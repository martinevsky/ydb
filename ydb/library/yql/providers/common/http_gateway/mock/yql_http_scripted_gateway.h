#pragma once

// H9: TScriptedHttpGateway, an IHTTPGateway fake for component tests (S3 actors, Solomon accessor,
// pool-cap pusher). Unlike IHTTPMockGateway it supports every method, records every call and lets
// the test decide when and on which thread each call completes.
//
// Buffered calls (Download without stream, Upload, Delete):
//   the buffered script returns a TResult to complete the call synchronously inside the gateway call,
//   or std::nullopt to leave it pending; Complete(callId, result) then finishes it from any thread.
//   Without a script every buffered call stays pending.
// Stream downloads:
//   the stream script receives a TStream handle; the test drives Start/Data/Finish on any thread, now or
//   later (the handle is also available through Streams()). Create the gateway with Make() (the cancel
//   hook records cancels through a weak reference to it). The returned cancel hook mirrors the real
//   gateway: it records the cancel, delivers OnFinish(CURLE_OK, {issue}) once if the stream has not
//   finished, and drops any later Data/Finish.
// Every call is recorded (method, url, headers, body, offset, size limit, per-url attempt, work scope);
// UpdatePoolCaps calls and cancel-hook invocations are recorded too. All methods are thread-safe;
// callbacks are never invoked under the gateway's lock.

#include <ydb/library/yql/providers/common/http_gateway/yql_http_gateway.h>

#include <util/datetime/base.h>
#include <util/generic/hash.h>
#include <util/generic/maybe.h>
#include <util/generic/vector.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

namespace NYql {

class TScriptedHttpGateway
    : public IHTTPGateway
    , public std::enable_shared_from_this<TScriptedHttpGateway>
{
public:
    using TPtr = std::shared_ptr<TScriptedHttpGateway>;

    enum class EMethod {
        Get,       // buffered Download
        GetStream, // streaming Download
        Post,      // Upload(put = false)
        Put,       // Upload(put = true)
        Delete,
    };

    struct TCall {
        ui64 Id = 0;
        EMethod Method = EMethod::Get;
        TString Url;
        THeaders Headers;
        TString Body; // Upload body, or the `data` argument of a buffered Download
        size_t Offset = 0;
        size_t SizeLimit = 0;
        ui32 Attempt = 0; // earlier calls with the same Method and Url
        TRetryPolicy::TPtr RetryPolicy;
        IHttpRequestContext::TPtr Context;
        std::optional<NDq::TWorkScope> WorkScope; // Context->GetWorkScope() when a context was given

        bool HasHeader(TStringBuf prefix) const; // any header field starting with `prefix`
    };

    class TStream {
    public:
        TStream(TCall call, TOnDownloadStart onStart, TOnNewDataPart onNewData, TOnDownloadFinish onFinish,
            ::NMonitoring::TDynamicCounters::TCounterPtr inflightCounter,
            std::shared_ptr<std::atomic_size_t> bufferedBytes, size_t threshold);

        const TCall& Call() const {
            return Call_;
        }
        // Each returns false (and does nothing) if the stream was cancelled or already finished.
        bool Start(CURLcode curlCode, long httpCode);
        bool Data(TString part);
        bool Finish(CURLcode curlCode, TIssues issues = {});

        bool IsCancelled() const;
        bool IsFinished() const;
        TMaybe<TIssue> CancelIssue() const;
        ui32 FinishCalls() const; // OnFinish deliveries (must stay <= 1)

        // Used by the cancel hook.
        void Cancel(TIssue issue);

    private:
        const TCall Call_;
        const TOnDownloadStart OnStart_;
        const TOnNewDataPart OnNewData_;
        const TOnDownloadFinish OnFinish_;
        const ::NMonitoring::TDynamicCounters::TCounterPtr InflightCounter_;
        const std::shared_ptr<std::atomic_size_t> BufferedBytes_;
        const size_t Threshold_;
        mutable std::mutex Mutex_;
        bool Cancelled_ = false;
        bool Finished_ = false;
        TMaybe<TIssue> CancelIssue_;
        ui32 FinishCalls_ = 0;
    };
    using TStreamPtr = std::shared_ptr<TStream>;

    struct TCancelRecord {
        ui64 CallId = 0;
        TIssue Issue;
        bool AfterFinish = false; // the stream had already finished when the hook ran
    };

    using TBufferedScript = std::function<std::optional<TResult>(const TCall&)>;
    using TStreamScript = std::function<void(const TStreamPtr&)>;

    static TPtr Make();

    void SetBufferedScript(TBufferedScript script);
    void SetStreamScript(TStreamScript script);
    void SetBuffersSizePerStream(ui64 size);

    // Completes a pending buffered call on the calling thread. Throws if `callId` is not pending.
    void Complete(ui64 callId, TResult result);
    TVector<ui64> PendingCalls() const;

    TVector<TCall> Calls() const;
    size_t CallCount() const;
    // Throws yexception if fewer than `count` calls were made within `guard`.
    void WaitForCalls(size_t count, TDuration guard = TDuration::Seconds(10)) const;
    TVector<TStreamPtr> Streams() const;
    TVector<TCancelRecord> Cancels() const;
    TVector<THashMap<NDq::TWorkScope, size_t>> PoolCapsUpdates() const;
    // Bytes held by live TCountedContent parts delivered through TStream::Data.
    size_t BufferedStreamBytes() const;

    // IHTTPGateway (default arguments repeat the interface's, so calls through TPtr compile the same way)
    void Upload(TString url, THeaders headers, TString body, TOnResult callback, bool put = false,
        TRetryPolicy::TPtr retryPolicy = TRetryPolicy::GetNoRetryPolicy(), IHttpRequestContext::TPtr context = nullptr) override;
    void Delete(TString url, THeaders headers, TOnResult callback,
        TRetryPolicy::TPtr retryPolicy = TRetryPolicy::GetNoRetryPolicy(), IHttpRequestContext::TPtr context = nullptr) override;
    void Download(TString url, THeaders headers, std::size_t offset, std::size_t sizeLimit, TOnResult callback,
        TString data = {}, TRetryPolicy::TPtr retryPolicy = TRetryPolicy::GetNoRetryPolicy(),
        IHttpRequestContext::TPtr context = nullptr) override;
    TCancelHook Download(TString url, THeaders headers, std::size_t offset, std::size_t sizeLimit,
        TOnDownloadStart onStart, TOnNewDataPart onNewData, TOnDownloadFinish onFinish,
        const ::NMonitoring::TDynamicCounters::TCounterPtr& inflightCounter, IHttpRequestContext::TPtr context = nullptr) override;
    ui64 GetBuffersSizePerStream() override;
    void UpdatePoolCaps(THashMap<NDq::TWorkScope, size_t> caps) override;

private:
    TCall Register(EMethod method, TString url, THeaders headers, TString body, size_t offset, size_t sizeLimit,
        TRetryPolicy::TPtr retryPolicy, IHttpRequestContext::TPtr context);
    void RunBuffered(TCall call, TOnResult callback);

    mutable std::mutex Mutex_;
    TBufferedScript BufferedScript_;
    TStreamScript StreamScript_;
    ui64 BuffersSizePerStream_ = 1 << 20;
    ui64 NextId_ = 0;
    TVector<TCall> Calls_;
    THashMap<std::pair<int, TString>, ui32> Attempts_;
    THashMap<ui64, TOnResult> Pending_;
    TVector<TStreamPtr> Streams_;
    TVector<TCancelRecord> Cancels_;
    TVector<THashMap<NDq::TWorkScope, size_t>> PoolCapsUpdates_;
    const std::shared_ptr<std::atomic_size_t> BufferedBytes_ = std::make_shared<std::atomic_size_t>(0);
};

} // namespace NYql
