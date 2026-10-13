#pragma once

#include <ydb/library/yql/providers/common/token_accessor/client/factory.h>
#include <library/cpp/testing/unittest/registar.h>
#include <vector>

namespace NYql::NTests {

// Explicitly controlled asynchronous provider: any synchronous use is a test failure.
class TAsyncCredentialsFactory : public IStructuredTokenCredentialsFactory {
public:
    struct TState {
        ui32 Calls = 0;
        bool Held = true;
        std::string Token;
        std::vector<NThreading::TPromise<std::string>> Pending;
    };
    const std::shared_ptr<TState> State = std::make_shared<TState>();

    class TProvider : public NYdb::ICredentialsProvider {
    public:
        explicit TProvider(std::shared_ptr<TState> state) : State(std::move(state)) {}
        std::string GetAuthInfo() const override { UNIT_FAIL("Synchronous credential acquisition"); return {}; }
        NThreading::TFuture<std::string> GetAuthInfoAsync() const override {
            ++State->Calls;
            if (!State->Held) return NThreading::MakeFuture(State->Token);
            auto promise = NThreading::NewPromise<std::string>();
            State->Pending.push_back(promise);
            return promise.GetFuture();
        }
        bool IsValid() const override { return true; }
    private:
        std::shared_ptr<TState> State;
    };
    class TFactory : public NYdb::ICredentialsProviderFactory {
    public:
        explicit TFactory(std::shared_ptr<TState> state) : State(std::move(state)) {}
        NYdb::TCredentialsProviderPtr CreateProvider() const override { return std::make_shared<TProvider>(State); }
        std::string GetClientIdentity() const override { return "asynchronous-test-provider"; }
    private:
        std::shared_ptr<TState> State;
    };
    std::shared_ptr<NYdb::ICredentialsProviderFactory> Create(const TString&, bool = false) override {
        return std::make_shared<TFactory>(State);
    }
    void Release(std::string token) {
        State->Held = false;
        State->Token = std::move(token);
        auto pending = std::move(State->Pending);
        for (auto& promise : pending) promise.SetValue(State->Token);
    }
    void Fail() {
        auto pending = std::move(State->Pending);
        for (auto& promise : pending) promise.SetException(TString("authorization denied"));
    }
};
} // namespace NYql::NTests
