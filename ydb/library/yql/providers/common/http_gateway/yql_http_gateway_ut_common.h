#pragma once

// Shared helpers of the THTTPMultiGateway transport tests (FQ transport test plan, target A).
// Test-only header: included by the *_ut.cpp files of http_gateway/ut, never by the library.

#include "yql_http_gateway.h"

#include <ydb/library/yql/providers/common/ut_helpers/transport/counters_inspector.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/refusing_port.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/wait.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/yql/gateway_scope.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/vector.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace NYql::NHttpGatewayUt {

inline const TDuration GUARD = TDuration::Seconds(10);

struct TOutcome {
    CURLcode CurlCode = CURLE_OK;
    long HttpCode = 0;
    TString Body;
    TString Issues;
};

// Collects the result of a buffered gateway call and counts callback invocations.
class TBufferedCall {
public:
    IHTTPGateway::TOnResult Callback() const {
        return [state = State_](IHTTPGateway::TResult&& result) {
            std::lock_guard lock(state->Mutex);
            ++state->Calls;
            state->Outcome.CurlCode = result.CurlResponseCode;
            state->Outcome.HttpCode = result.Content.HttpResponseCode;
            state->Outcome.Body = result.Content.Extract();
            state->Outcome.Issues = result.Issues.ToOneLineString();
            state->Done.notify_all();
        };
    }

    TOutcome Wait(TDuration guard = GUARD) const {
        std::unique_lock lock(State_->Mutex);
        const bool done = State_->Done.wait_for(lock, std::chrono::microseconds(guard.MicroSeconds()), [&]() {
            return State_->Calls > 0;
        });
        UNIT_ASSERT_C(done, "gateway callback not called within guard " << guard);
        return State_->Outcome;
    }

    ui32 Calls() const {
        std::lock_guard lock(State_->Mutex);
        return State_->Calls;
    }

private:
    struct TState {
        std::mutex Mutex;
        std::condition_variable Done;
        ui32 Calls = 0;
        TOutcome Outcome;
    };
    std::shared_ptr<TState> State_ = std::make_shared<TState>();
};

inline TOutcome Download(const NTransportTest::TGatewayScope& gateway, const TString& url,
    IHTTPGateway::THeaders headers = {}, size_t sizeLimit = 0, TBufferedCall call = {})
{
    gateway->Download(url, std::move(headers), 0, sizeLimit, call.Callback());
    return call.Wait();
}

inline void Finish(NTransportTest::TGatewayScope& gateway) {
    gateway.Inspector().AssertSettled(); // T-OBS-3
    gateway.Close();
}

// Sync point: the curl thread has run at least `cycles` more perform cycles after this call. Anything
// the gateway would do "on the next FillHandlers" has happened (or will never happen) afterwards.
inline void WaitPerformCycles(const NTransportTest::TGatewayScope& gateway, i64 cycles = 3) {
    const i64 start = gateway.Inspector().Get("PerformCycles");
    NTransportTest::WaitUntil([&]() {
        return gateway.Inspector().Get("PerformCycles") >= start + cycles;
    }, GUARD, "the gateway perform loop advances");
}

// Records a streaming download: parts are held (backpressure) until Release(), then consumed on arrival.
class TStreamConsumer {
public:
    IHTTPGateway::TOnDownloadStart OnStart() {
        return [state = State_](CURLcode, long httpCode) {
            std::lock_guard lock(state->Mutex);
            state->HttpCode = httpCode;
        };
    }

    IHTTPGateway::TOnNewDataPart OnData() {
        return [state = State_](IHTTPGateway::TCountedContent&& part) {
            std::lock_guard lock(state->Mutex);
            state->Bytes += part.size();
            state->MaxPart = std::max<ui64>(state->MaxPart, part.size());
            ++state->Parts;
            if (state->Hold) {
                state->Held.push_back(std::move(part));
            }
        };
    }

    IHTTPGateway::TOnDownloadFinish OnFinish() {
        return [state = State_](CURLcode code, TIssues issues) {
            std::lock_guard lock(state->Mutex);
            ++state->Finishes;
            state->FinishCode = code;
            state->FinishIssues = issues.ToOneLineString();
            state->Done.notify_all();
        };
    }

    // Stop holding and drop every held part (the gateway resumes the stream).
    void Release() {
        TVector<IHTTPGateway::TCountedContent> held;
        {
            std::lock_guard lock(State_->Mutex);
            State_->Hold = false;
            held.swap(State_->Held);
        }
    }

    // Waits for OnFinish; returns false if it did not come within `window` (no assertion).
    bool WaitFinish(TDuration window) const {
        std::unique_lock lock(State_->Mutex);
        return State_->Done.wait_for(lock, std::chrono::microseconds(window.MicroSeconds()), [&]() {
            return State_->Finishes > 0;
        });
    }

    ui64 Bytes() const { std::lock_guard lock(State_->Mutex); return State_->Bytes; }
    ui64 MaxPart() const { std::lock_guard lock(State_->Mutex); return State_->MaxPart; }
    ui64 Parts() const { std::lock_guard lock(State_->Mutex); return State_->Parts; }
    ui32 Finishes() const { std::lock_guard lock(State_->Mutex); return State_->Finishes; }
    CURLcode FinishCode() const { std::lock_guard lock(State_->Mutex); return State_->FinishCode; }
    TString FinishIssues() const { std::lock_guard lock(State_->Mutex); return State_->FinishIssues; }
    long HttpCode() const { std::lock_guard lock(State_->Mutex); return State_->HttpCode; }

private:
    struct TState {
        std::mutex Mutex;
        std::condition_variable Done;
        bool Hold = true;
        TVector<IHTTPGateway::TCountedContent> Held;
        ui64 Bytes = 0;
        ui64 MaxPart = 0;
        ui64 Parts = 0;
        ui32 Finishes = 0;
        CURLcode FinishCode = CURLE_OK;
        TString FinishIssues;
        long HttpCode = 0;
    };
    std::shared_ptr<TState> State_ = std::make_shared<TState>();
};

inline IHTTPGateway::TCancelHook StartStream(const NTransportTest::TGatewayScope& gateway, const TString& url,
    TStreamConsumer& consumer, IHttpRequestContext::TPtr context = nullptr)
{
    return gateway->Download(url, {}, 0, 0, consumer.OnStart(), consumer.OnData(), consumer.OnFinish(), nullptr,
        std::move(context));
}

} // namespace NYql::NHttpGatewayUt
