#pragma once

// H8: a thread-safe manual clock for seams that take a `std::function<TInstant()>` (S1, S2).

#include <util/datetime/base.h>

#include <functional>
#include <mutex>

namespace NYql::NTransportTest {

class TManualClock {
public:
    explicit TManualClock(TInstant start = TInstant::Seconds(1'700'000'000))
        : Now_(start)
    {
    }

    TInstant Now() const {
        std::lock_guard lock(Mutex_);
        return Now_;
    }

    void Advance(TDuration delta) {
        std::lock_guard lock(Mutex_);
        Now_ += delta;
    }

    void Set(TInstant now) {
        std::lock_guard lock(Mutex_);
        Now_ = now;
    }

    // The returned function references this clock: keep the clock alive while it is used.
    std::function<TInstant()> AsFunction() const {
        return [this]() {
            return Now();
        };
    }

private:
    mutable std::mutex Mutex_;
    TInstant Now_;
};

} // namespace NYql::NTransportTest
