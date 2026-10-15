#pragma once

#include <util/datetime/base.h>

namespace NKikimr::NIamDelegation {

// Shared bounds for the actual standard backoffs and consumer deadlines.
// TBackoff adds positive jitter of less than one quarter of its base delay.
struct TRetrySettings {
    static constexpr TDuration InitialDelay = TDuration::Seconds(1);
    static constexpr TDuration MaxDelay = TDuration::Minutes(1);
    static constexpr TDuration RpcInitialDelay = TDuration::MilliSeconds(200);
    static constexpr TDuration RpcMaxDelay = TDuration::Seconds(10);
    static constexpr TDuration RpcResponseSlack = TDuration::Seconds(1);
    static constexpr TDuration CompletionSlack = TDuration::Seconds(1);

    static constexpr TDuration MaxJitteredDelay(TDuration base) {
        return base + base / 4;
    }
};

// Reconstruct the standard capped sequence from durable history with bounded
// work, including a saturated failure counter. Zero denotes pending progress.
TDuration RevocationRetryDelay(ui32 consecutiveFailures);

// Standard positive jitter in an earlier window ending at the healthy target.
// The one-second floor avoids busy refreshes for almost unusable tokens.
TDuration SuccessfulRefreshDelay(TDuration target);

} // namespace NKikimr::NIamDelegation
