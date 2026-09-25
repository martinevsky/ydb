#include "wait.h"

#include <util/generic/yexception.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace NYql::NTransportTest {

void WaitUntil(const std::function<bool()>& predicate, TDuration guard, TStringBuf what) {
    const TInstant deadline = guard.ToDeadLine();
    std::mutex mutex;
    std::condition_variable pause;
    while (!predicate()) {
        if (TInstant::Now() >= deadline) {
            ythrow yexception() << "guard " << guard << " expired while waiting for: " << what;
        }
        std::unique_lock lock(mutex);
        pause.wait_for(lock, std::chrono::milliseconds(1));
    }
}

} // namespace NYql::NTransportTest
