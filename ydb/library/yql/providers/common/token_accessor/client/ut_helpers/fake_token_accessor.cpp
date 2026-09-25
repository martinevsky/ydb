#include "fake_token_accessor.h"

#include <ydb/library/yql/providers/common/token_accessor/grpc/token_accessor_pb.grpc.pb.h>

#include <util/generic/yexception.h>
#include <util/string/builder.h>

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

namespace NYql::NTransportTest {

TTokenReply TTokenReply::Ok(TString token) {
    TTokenReply reply;
    reply.Token = std::move(token);
    return reply;
}

TTokenReply TTokenReply::Fail(grpc::StatusCode code, TString message) {
    Y_ENSURE(code != grpc::StatusCode::OK, "TTokenReply::Fail needs a non-OK code");
    TTokenReply reply;
    reply.Code = code;
    reply.Message = std::move(message);
    return reply;
}

TTokenReply TTokenReply::Held() const {
    TTokenReply reply = *this;
    reply.Hold = true;
    return reply;
}

struct TFakeTokenAccessor::TState {
    mutable std::mutex Mutex;
    std::condition_variable Changed;

    TString DefaultToken;
    THashMap<TString, TString> Tokens;
    ui64 FailuresLeft = 0;
    grpc::StatusCode FailureCode = grpc::StatusCode::UNAVAILABLE;
    TScript Script;
    bool Gated = false;
    ui64 ReleaseGeneration = 0;
    bool Stopping = false;

    TVector<TTokenAccessorCall> Calls;
    THashMap<TString, ui64> Attempts;
    ui64 HeldNow = 0;
};

class TFakeTokenAccessor::TService final : public TokenAccessorService::Service {
public:
    explicit TService(std::shared_ptr<TState> state)
        : State_(std::move(state))
    {}

    grpc::Status GetToken(grpc::ServerContext*, const GetTokenRequest* request, GetTokenResponse* response) override {
        TTokenAccessorCall call;
        call.Type = request->type();
        call.TokenId = request->token_id();
        call.Signature = request->signature();

        std::unique_lock lock(State_->Mutex);
        call.Index = State_->Calls.size();
        call.Attempt = ++State_->Attempts[call.TokenId];

        TTokenReply reply;
        if (State_->Script) {
            reply = State_->Script(call);
        } else if (State_->FailuresLeft > 0) {
            --State_->FailuresLeft;
            reply = TTokenReply::Fail(State_->FailureCode);
        } else {
            const auto* token = State_->Tokens.FindPtr(call.TokenId);
            reply = TTokenReply::Ok(token ? *token : State_->DefaultToken);
        }

        call.Held = State_->Gated || reply.Hold;
        State_->Calls.push_back(call);
        State_->Changed.notify_all();

        if (call.Held) {
            // Latch: the server thread waits for the test's Release() (or Shutdown()).
            ++State_->HeldNow;
            State_->Changed.notify_all();
            const ui64 generation = State_->ReleaseGeneration;
            State_->Changed.wait(lock, [&]() {
                return State_->ReleaseGeneration != generation || State_->Stopping;
            });
            --State_->HeldNow;
            State_->Changed.notify_all();
        }
        lock.unlock();

        if (reply.Code != grpc::StatusCode::OK) {
            return grpc::Status(reply.Code, reply.Message);
        }
        response->set_token(reply.Token);
        return grpc::Status::OK;
    }

private:
    const std::shared_ptr<TState> State_;
};

TFakeTokenAccessor::TFakeTokenAccessor(TString defaultToken)
    : State_(std::make_shared<TState>())
    , Service_(std::make_unique<TService>(State_))
{
    State_->DefaultToken = std::move(defaultToken);

    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &Port_);
    builder.RegisterService(Service_.get());
    Server_ = builder.BuildAndStart();
    Y_ENSURE(Server_ && Port_ > 0, "TFakeTokenAccessor: failed to start the gRPC server on 127.0.0.1:0");
}

TFakeTokenAccessor::~TFakeTokenAccessor() {
    Shutdown();
}

ui16 TFakeTokenAccessor::Port() const {
    return static_cast<ui16>(Port_);
}

TString TFakeTokenAccessor::Endpoint() const {
    return TStringBuilder() << "127.0.0.1:" << Port_;
}

void TFakeTokenAccessor::SetToken(const TString& tokenId, TString token) {
    std::lock_guard lock(State_->Mutex);
    State_->Tokens[tokenId] = std::move(token);
}

void TFakeTokenAccessor::FailNThenSucceed(ui64 failures, grpc::StatusCode code) {
    Y_ENSURE(code != grpc::StatusCode::OK, "FailNThenSucceed needs a non-OK code");
    std::lock_guard lock(State_->Mutex);
    State_->FailuresLeft = failures;
    State_->FailureCode = code;
}

void TFakeTokenAccessor::SetScript(TScript script) {
    std::lock_guard lock(State_->Mutex);
    State_->Script = std::move(script);
}

void TFakeTokenAccessor::GateReplies() {
    std::lock_guard lock(State_->Mutex);
    State_->Gated = true;
}

void TFakeTokenAccessor::Release() {
    std::lock_guard lock(State_->Mutex);
    State_->Gated = false;
    ++State_->ReleaseGeneration;
    State_->Changed.notify_all();
}

TVector<TTokenAccessorCall> TFakeTokenAccessor::Calls() const {
    std::lock_guard lock(State_->Mutex);
    return State_->Calls;
}

ui64 TFakeTokenAccessor::CallCount() const {
    std::lock_guard lock(State_->Mutex);
    return State_->Calls.size();
}

ui64 TFakeTokenAccessor::CallCount(const TString& tokenId) const {
    std::lock_guard lock(State_->Mutex);
    const auto* attempts = State_->Attempts.FindPtr(tokenId);
    return attempts ? *attempts : 0;
}

ui64 TFakeTokenAccessor::HeldNow() const {
    std::lock_guard lock(State_->Mutex);
    return State_->HeldNow;
}

void TFakeTokenAccessor::WaitFor(const std::function<bool()>& predicate, TDuration guard, const TString& what) const {
    std::unique_lock lock(State_->Mutex);
    const bool reached = State_->Changed.wait_for(lock, std::chrono::microseconds(guard.MicroSeconds()), predicate);
    Y_ENSURE(reached, "guard of " << guard << " expired waiting until " << what);
}

void TFakeTokenAccessor::WaitForCalls(ui64 count, TDuration guard) const {
    WaitFor([&]() {
        return State_->Calls.size() >= count;
    }, guard, TStringBuilder() << "TFakeTokenAccessor received " << count << " GetToken call(s)");
}

void TFakeTokenAccessor::WaitForCalls(const TString& tokenId, ui64 count, TDuration guard) const {
    WaitFor([&]() {
        const auto* attempts = State_->Attempts.FindPtr(tokenId);
        return attempts && *attempts >= count;
    }, guard, TStringBuilder() << "TFakeTokenAccessor received " << count << " GetToken call(s) for " << tokenId);
}

void TFakeTokenAccessor::WaitForHeldCalls(ui64 count, TDuration guard) const {
    WaitFor([&]() {
        return State_->HeldNow >= count;
    }, guard, TStringBuilder() << "TFakeTokenAccessor holds " << count << " GetToken call(s)");
}

void TFakeTokenAccessor::Shutdown() {
    if (!Server_) {
        return;
    }
    {
        std::lock_guard lock(State_->Mutex);
        State_->Stopping = true;
        State_->Changed.notify_all();
    }
    Server_->Shutdown();
    Server_->Wait();
    Server_.reset();
}

} // namespace NYql::NTransportTest
