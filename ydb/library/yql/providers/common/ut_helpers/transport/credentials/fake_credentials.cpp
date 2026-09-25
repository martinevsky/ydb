#include "fake_credentials.h"

#include <util/generic/yexception.h>

namespace NYql::NTransportTest {

TFakeCredentialsProvider::TFakeCredentialsProvider(TString token)
    : Token_(std::move(token))
{
}

TFakeCredentialsProvider::~TFakeCredentialsProvider() {
    std::unique_lock lock(Mutex_);
    Destroying_ = true;
    Changed_.notify_all();
    Changed_.wait(lock, [this]() {
        return BlockedNow_ == 0;
    });
}

void TFakeCredentialsProvider::SetToken(TString token) {
    std::lock_guard lock(Mutex_);
    Token_ = std::move(token);
}

void TFakeCredentialsProvider::FailNext(ui32 count, TString message) {
    std::lock_guard lock(Mutex_);
    FailuresLeft_ = count;
    FailureMessage_ = std::move(message);
}

void TFakeCredentialsProvider::Block() {
    std::lock_guard lock(Mutex_);
    Blocked_ = true;
}

void TFakeCredentialsProvider::Unblock() {
    std::lock_guard lock(Mutex_);
    Blocked_ = false;
    Changed_.notify_all();
}

ui64 TFakeCredentialsProvider::Calls() const {
    std::lock_guard lock(Mutex_);
    return Calls_;
}

ui64 TFakeCredentialsProvider::BlockedCalls() const {
    std::lock_guard lock(Mutex_);
    return BlockedNow_;
}

void TFakeCredentialsProvider::WaitForBlockedCalls(ui64 count, TDuration guard) const {
    std::unique_lock lock(Mutex_);
    const bool reached = Changed_.wait_for(lock, std::chrono::microseconds(guard.MicroSeconds()), [&]() {
        return BlockedNow_ >= count;
    });
    if (!reached) {
        ythrow yexception() << "guard " << guard << " expired waiting for " << count << " blocked GetAuthInfo() call(s); "
            << BlockedNow_ << " blocked";
    }
}

std::string TFakeCredentialsProvider::GetAuthInfo() const {
    std::unique_lock lock(Mutex_);
    ++Calls_;
    if (Blocked_ && !Destroying_) {
        ++BlockedNow_;
        Changed_.notify_all();
        Changed_.wait(lock, [this]() {
            return !Blocked_ || Destroying_;
        });
        --BlockedNow_;
        Changed_.notify_all();
    }
    if (FailuresLeft_ > 0) {
        --FailuresLeft_;
        ythrow yexception() << FailureMessage_;
    }
    return Token_;
}

bool TFakeCredentialsProvider::IsValid() const {
    return true;
}

} // namespace NYql::NTransportTest
