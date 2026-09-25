#include "yql_http_scripted_gateway.h"

#include <util/generic/algorithm.h>
#include <util/generic/yexception.h>
#include <util/string/builder.h>

#include <chrono>
#include <condition_variable>

namespace NYql {

bool TScriptedHttpGateway::TCall::HasHeader(TStringBuf prefix) const {
    for (const auto& field : Headers.Fields) {
        if (TStringBuf(field).StartsWith(prefix)) {
            return true;
        }
    }
    return false;
}

TScriptedHttpGateway::TStream::TStream(TCall call, TOnDownloadStart onStart, TOnNewDataPart onNewData,
    TOnDownloadFinish onFinish, ::NMonitoring::TDynamicCounters::TCounterPtr inflightCounter,
    std::shared_ptr<std::atomic_size_t> bufferedBytes, size_t threshold)
    : Call_(std::move(call))
    , OnStart_(std::move(onStart))
    , OnNewData_(std::move(onNewData))
    , OnFinish_(std::move(onFinish))
    , InflightCounter_(std::move(inflightCounter))
    , BufferedBytes_(std::move(bufferedBytes))
    , Threshold_(threshold)
{
}

bool TScriptedHttpGateway::TStream::Start(CURLcode curlCode, long httpCode) {
    {
        std::lock_guard lock(Mutex_);
        if (Cancelled_ || Finished_) {
            return false;
        }
    }
    OnStart_(curlCode, httpCode);
    return true;
}

bool TScriptedHttpGateway::TStream::Data(TString part) {
    {
        std::lock_guard lock(Mutex_);
        if (Cancelled_ || Finished_) {
            return false;
        }
    }
    OnNewData_(TCountedContent(std::move(part), BufferedBytes_, InflightCounter_, {}, Threshold_));
    return true;
}

bool TScriptedHttpGateway::TStream::Finish(CURLcode curlCode, TIssues issues) {
    {
        std::lock_guard lock(Mutex_);
        if (Cancelled_ || Finished_) {
            return false;
        }
        Finished_ = true;
        ++FinishCalls_;
    }
    OnFinish_(curlCode, std::move(issues));
    return true;
}

void TScriptedHttpGateway::TStream::Cancel(TIssue issue) {
    bool deliver = false;
    {
        std::lock_guard lock(Mutex_);
        if (Cancelled_) {
            return;
        }
        Cancelled_ = true;
        CancelIssue_ = issue;
        if (!Finished_) {
            Finished_ = true;
            ++FinishCalls_;
            deliver = true;
        }
    }
    if (deliver) {
        OnFinish_(CURLE_OK, TIssues{issue});
    }
}

bool TScriptedHttpGateway::TStream::IsCancelled() const {
    std::lock_guard lock(Mutex_);
    return Cancelled_;
}

bool TScriptedHttpGateway::TStream::IsFinished() const {
    std::lock_guard lock(Mutex_);
    return Finished_;
}

TMaybe<TIssue> TScriptedHttpGateway::TStream::CancelIssue() const {
    std::lock_guard lock(Mutex_);
    return CancelIssue_;
}

ui32 TScriptedHttpGateway::TStream::FinishCalls() const {
    std::lock_guard lock(Mutex_);
    return FinishCalls_;
}

TScriptedHttpGateway::TPtr TScriptedHttpGateway::Make() {
    return std::make_shared<TScriptedHttpGateway>();
}

void TScriptedHttpGateway::SetBufferedScript(TBufferedScript script) {
    std::lock_guard lock(Mutex_);
    BufferedScript_ = std::move(script);
}

void TScriptedHttpGateway::SetStreamScript(TStreamScript script) {
    std::lock_guard lock(Mutex_);
    StreamScript_ = std::move(script);
}

void TScriptedHttpGateway::SetBuffersSizePerStream(ui64 size) {
    std::lock_guard lock(Mutex_);
    BuffersSizePerStream_ = size;
}

TScriptedHttpGateway::TCall TScriptedHttpGateway::Register(EMethod method, TString url, THeaders headers, TString body,
    size_t offset, size_t sizeLimit, TRetryPolicy::TPtr retryPolicy, IHttpRequestContext::TPtr context)
{
    TCall call;
    call.Method = method;
    call.Url = std::move(url);
    call.Headers = std::move(headers);
    call.Body = std::move(body);
    call.Offset = offset;
    call.SizeLimit = sizeLimit;
    call.RetryPolicy = std::move(retryPolicy);
    if (context) {
        call.WorkScope = context->GetWorkScope();
    }
    call.Context = std::move(context);
    std::lock_guard lock(Mutex_);
    call.Id = NextId_++;
    call.Attempt = Attempts_[std::make_pair(static_cast<int>(method), call.Url)]++;
    Calls_.push_back(call);
    return call;
}

void TScriptedHttpGateway::RunBuffered(TCall call, TOnResult callback) {
    TBufferedScript script;
    {
        std::lock_guard lock(Mutex_);
        script = BufferedScript_;
    }
    std::optional<TResult> result;
    if (script) {
        result = script(call);
    }
    if (result) {
        callback(std::move(*result));
        return;
    }
    std::lock_guard lock(Mutex_);
    Pending_.emplace(call.Id, std::move(callback));
}

void TScriptedHttpGateway::Complete(ui64 callId, TResult result) {
    TOnResult callback;
    {
        std::lock_guard lock(Mutex_);
        const auto it = Pending_.find(callId);
        Y_ENSURE(it != Pending_.end(), "call " << callId << " is not pending");
        callback = std::move(it->second);
        Pending_.erase(it);
    }
    callback(std::move(result));
}

TVector<ui64> TScriptedHttpGateway::PendingCalls() const {
    std::lock_guard lock(Mutex_);
    TVector<ui64> ids;
    for (const auto& [id, _] : Pending_) {
        ids.push_back(id);
    }
    Sort(ids);
    return ids;
}

TVector<TScriptedHttpGateway::TCall> TScriptedHttpGateway::Calls() const {
    std::lock_guard lock(Mutex_);
    return Calls_;
}

size_t TScriptedHttpGateway::CallCount() const {
    std::lock_guard lock(Mutex_);
    return Calls_.size();
}

void TScriptedHttpGateway::WaitForCalls(size_t count, TDuration guard) const {
    const TInstant deadline = guard.ToDeadLine();
    std::mutex pauseMutex;
    std::condition_variable pause;
    while (CallCount() < count) {
        if (TInstant::Now() >= deadline) {
            ythrow yexception() << "guard " << guard << " expired waiting for " << count << " gateway call(s); got " << CallCount();
        }
        std::unique_lock lock(pauseMutex);
        pause.wait_for(lock, std::chrono::milliseconds(1));
    }
}

TVector<TScriptedHttpGateway::TStreamPtr> TScriptedHttpGateway::Streams() const {
    std::lock_guard lock(Mutex_);
    return Streams_;
}

TVector<TScriptedHttpGateway::TCancelRecord> TScriptedHttpGateway::Cancels() const {
    std::lock_guard lock(Mutex_);
    return Cancels_;
}

TVector<THashMap<NDq::TWorkScope, size_t>> TScriptedHttpGateway::PoolCapsUpdates() const {
    std::lock_guard lock(Mutex_);
    return PoolCapsUpdates_;
}

size_t TScriptedHttpGateway::BufferedStreamBytes() const {
    return BufferedBytes_->load();
}

void TScriptedHttpGateway::Upload(TString url, THeaders headers, TString body, TOnResult callback, bool put,
    TRetryPolicy::TPtr retryPolicy, IHttpRequestContext::TPtr context)
{
    auto call = Register(put ? EMethod::Put : EMethod::Post, std::move(url), std::move(headers), std::move(body), 0, 0,
        std::move(retryPolicy), std::move(context));
    RunBuffered(std::move(call), std::move(callback));
}

void TScriptedHttpGateway::Delete(TString url, THeaders headers, TOnResult callback, TRetryPolicy::TPtr retryPolicy,
    IHttpRequestContext::TPtr context)
{
    auto call = Register(EMethod::Delete, std::move(url), std::move(headers), {}, 0, 0, std::move(retryPolicy), std::move(context));
    RunBuffered(std::move(call), std::move(callback));
}

void TScriptedHttpGateway::Download(TString url, THeaders headers, std::size_t offset, std::size_t sizeLimit,
    TOnResult callback, TString data, TRetryPolicy::TPtr retryPolicy, IHttpRequestContext::TPtr context)
{
    auto call = Register(EMethod::Get, std::move(url), std::move(headers), std::move(data), offset, sizeLimit,
        std::move(retryPolicy), std::move(context));
    RunBuffered(std::move(call), std::move(callback));
}

IHTTPGateway::TCancelHook TScriptedHttpGateway::Download(TString url, THeaders headers, std::size_t offset,
    std::size_t sizeLimit, TOnDownloadStart onStart, TOnNewDataPart onNewData, TOnDownloadFinish onFinish,
    const ::NMonitoring::TDynamicCounters::TCounterPtr& inflightCounter, IHttpRequestContext::TPtr context)
{
    auto call = Register(EMethod::GetStream, std::move(url), std::move(headers), {}, offset, sizeLimit, nullptr, std::move(context));
    TStreamScript script;
    TStreamPtr stream;
    {
        std::lock_guard lock(Mutex_);
        stream = std::make_shared<TStream>(call, std::move(onStart), std::move(onNewData), std::move(onFinish),
            inflightCounter, BufferedBytes_, BuffersSizePerStream_);
        Streams_.push_back(stream);
        script = StreamScript_;
    }
    if (script) {
        script(stream);
    }
    const std::weak_ptr<TStream> weakStream = stream;
    const std::weak_ptr<TScriptedHttpGateway> weakSelf = weak_from_this();
    const ui64 callId = call.Id;
    return [weakStream, weakSelf, callId](TIssue issue) {
        const auto stream = weakStream.lock();
        if (const auto self = weakSelf.lock()) {
            std::lock_guard lock(self->Mutex_);
            self->Cancels_.push_back({callId, issue, stream && stream->IsFinished() && !stream->IsCancelled()});
        }
        if (stream) {
            stream->Cancel(std::move(issue));
        }
    };
}

ui64 TScriptedHttpGateway::GetBuffersSizePerStream() {
    std::lock_guard lock(Mutex_);
    return BuffersSizePerStream_;
}

void TScriptedHttpGateway::UpdatePoolCaps(THashMap<NDq::TWorkScope, size_t> caps) {
    std::lock_guard lock(Mutex_);
    PoolCapsUpdates_.push_back(std::move(caps));
}

} // namespace NYql
