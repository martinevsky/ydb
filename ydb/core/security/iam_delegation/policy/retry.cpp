#include "retry.h"

#include <ydb/core/util/backoff.h>
#include <util/generic/utility.h>

namespace NKikimr::NIamDelegation {

TDuration RevocationRetryDelay(ui32 consecutiveFailures) {
    TBackoffTimer timer(TRetrySettings::InitialDelay.MilliSeconds(), TRetrySettings::MaxDelay.MilliSeconds());
    TDuration base = timer.Next();
    for (ui32 iteration = 1; iteration < consecutiveFailures && base < TRetrySettings::MaxDelay; ++iteration) {
        base = timer.Next();
    }
    return TBackoff(base, base).Next();
}

TDuration SuccessfulRefreshDelay(TDuration target) {
    // TBackoff stores its base in milliseconds. Round down so neither this
    // conversion nor positive jitter can extend the existing refresh target.
    const auto base = TDuration::MilliSeconds((target * 4 / 5).MilliSeconds());
    return Max(TRetrySettings::InitialDelay, TBackoff(base, base).Next());
}

} // namespace NKikimr::NIamDelegation
