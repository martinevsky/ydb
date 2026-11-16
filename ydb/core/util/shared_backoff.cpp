#include "shared_backoff.h"

namespace NKikimr {
TSharedBackoff::TSharedBackoff(TDuration initialDelay, TDuration maxDelay)
    : Backoff(initialDelay, maxDelay)
{
    Y_ABORT_UNLESS(initialDelay.MilliSeconds() > 0);
}

std::optional<ui64> TSharedBackoff::TryAcquire(NActors::TMonotonic, TDuration) {
    return std::nullopt;
}
TSharedBackoff::EUpdate TSharedBackoff::OnFailure(ui64, NActors::TMonotonic) {
    return EUpdate::Stale;
}
TSharedBackoff::EUpdate TSharedBackoff::OnSuccess(ui64) {
    return EUpdate::Stale;
}
} // namespace NKikimr
