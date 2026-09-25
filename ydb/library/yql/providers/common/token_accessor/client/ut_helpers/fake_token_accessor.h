#pragma once

// H5: TFakeTokenAccessor, an in-process gRPC TokenAccessorService on 127.0.0.1:<port 0>
// (test_plan.md §3). Framework-neutral: failures are reported by throwing yexception
// (see ydb/library/yql/providers/common/ut_helpers/transport/README.md). No sleeps: held replies
// wait on a latch that the test opens.
//
//   TFakeTokenAccessor accessor;                          // answers every GetToken with "SA-TOKEN"
//   accessor.GateReplies();                               // hold every reply until Release()
//   accessor.WaitForCalls(1);                             // CallArrived: a GetToken reached the server
//   accessor.Release();                                   // held replies are answered now
//   accessor.FailNThenSucceed(2, grpc::StatusCode::UNAVAILABLE); // the next 2 calls fail
//   accessor.SetScript([](const TTokenAccessorCall& call) { ... return TTokenReply::Ok("T"); });
//
// Reply selection for a call: the script if one is set, else a pending FailNThenSucceed failure,
// else OK with the token for the call's service account (SetToken) or the default token.
// A call is held when GateReplies() is active or its reply has Hold = true; every held call is
// answered at the next Release() (or at Shutdown()).

#include <util/datetime/base.h>
#include <util/generic/hash.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>

#include <grpcpp/server.h>
#include <grpcpp/support/status.h>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

namespace NYql::NTransportTest {

struct TTokenAccessorCall {
    ui64 Index = 0;         // 0-based, over all calls
    ui64 Attempt = 0;       // 1-based, per service account id (TokenId)
    int Type = 0;           // GetTokenRequest::Type
    TString TokenId;        // the service account id
    TString Signature;
    bool Held = false;
};

struct TTokenReply {
    grpc::StatusCode Code = grpc::StatusCode::OK;
    TString Message;
    TString Token;
    bool Hold = false;      // hold this reply until the next Release()

    static TTokenReply Ok(TString token);
    static TTokenReply Fail(grpc::StatusCode code, TString message = "fake token accessor failure");
    TTokenReply Held() const;
};

class TFakeTokenAccessor {
public:
    using TScript = std::function<TTokenReply(const TTokenAccessorCall& call)>;

    explicit TFakeTokenAccessor(TString defaultToken = "SA-TOKEN");
    ~TFakeTokenAccessor();

    TFakeTokenAccessor(const TFakeTokenAccessor&) = delete;
    TFakeTokenAccessor& operator=(const TFakeTokenAccessor&) = delete;

    ui16 Port() const;
    // "127.0.0.1:<port>" for TGRpcClientConfig::Locator.
    TString Endpoint() const;

    void SetToken(const TString& tokenId, TString token);
    void FailNThenSucceed(ui64 failures, grpc::StatusCode code = grpc::StatusCode::UNAVAILABLE);
    void SetScript(TScript script);

    // Latch: while gated, every arriving call is held (not answered).
    void GateReplies();
    // Opens the gate and answers every held call.
    void Release();

    TVector<TTokenAccessorCall> Calls() const;
    ui64 CallCount() const;
    ui64 CallCount(const TString& tokenId) const;
    ui64 HeldNow() const;

    // CallArrived sync points (throw yexception on guard expiry).
    void WaitForCalls(ui64 count, TDuration guard = TDuration::Seconds(10)) const;
    void WaitForCalls(const TString& tokenId, ui64 count, TDuration guard = TDuration::Seconds(10)) const;
    void WaitForHeldCalls(ui64 count, TDuration guard = TDuration::Seconds(10)) const;

    // Answers held calls, then shuts the server down and waits for it. Idempotent.
    void Shutdown();

private:
    class TService;
    struct TState;

    void WaitFor(const std::function<bool()>& predicate, TDuration guard, const TString& what) const;

    const std::shared_ptr<TState> State_;
    std::unique_ptr<TService> Service_;
    std::unique_ptr<grpc::Server> Server_;
    int Port_ = 0;
};

} // namespace NYql::NTransportTest
