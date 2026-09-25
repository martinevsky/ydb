#pragma once

// H10: TFakeCredentialsProvider, a scriptable NYdb::ICredentialsProvider.
//
//   auto creds = std::make_shared<TFakeCredentialsProvider>("SECRET");
//   creds->FailNext(2);            // the next 2 GetAuthInfo() calls throw "IAM-token not ready yet"
//   creds->Block();                // GetAuthInfo() blocks until Unblock() (or destruction)
//   creds->WaitForBlockedCalls(1); // sync point: a caller is blocked inside GetAuthInfo()
//   creds->Unblock();
//   creds->Calls();                // number of GetAuthInfo() calls so far
//
// Lives in its own library because NYdb::ICredentialsProvider pulls the YDB SDK credentials target.

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/credentials/credentials.h>

#include <util/datetime/base.h>
#include <util/generic/string.h>

#include <condition_variable>
#include <mutex>

namespace NYql::NTransportTest {

class TFakeCredentialsProvider final : public NYdb::ICredentialsProvider {
public:
    explicit TFakeCredentialsProvider(TString token);
    ~TFakeCredentialsProvider() override;

    void SetToken(TString token);
    // The next `count` calls throw yexception(message).
    void FailNext(ui32 count, TString message = "IAM-token not ready yet");
    void Block();
    void Unblock();

    ui64 Calls() const;
    ui64 BlockedCalls() const;
    // Waits until at least `count` callers are blocked in GetAuthInfo(); throws on guard expiry.
    void WaitForBlockedCalls(ui64 count = 1, TDuration guard = TDuration::Seconds(10)) const;

    std::string GetAuthInfo() const override;
    bool IsValid() const override;

private:
    mutable std::mutex Mutex_;
    mutable std::condition_variable Changed_;
    TString Token_;
    mutable ui32 FailuresLeft_ = 0;
    TString FailureMessage_;
    bool Blocked_ = false;
    bool Destroying_ = false;
    mutable ui64 Calls_ = 0;
    mutable ui64 BlockedNow_ = 0;
};

} // namespace NYql::NTransportTest
