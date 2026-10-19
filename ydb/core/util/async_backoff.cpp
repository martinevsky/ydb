#include "async_backoff.h"

namespace NKikimr {

TAsyncBackoff::TAsyncBackoff(TDuration initialDelay, TDuration maxDelay)
    : Backoff(initialDelay, maxDelay)
{}

NActors::async<ui64> TAsyncBackoff::Wait(TDuration) {
    co_return Version;
}

TAsyncBackoff::EUpdate TAsyncBackoff::OnFailure(ui64) {
    return EUpdate::Applied;
}

TAsyncBackoff::EUpdate TAsyncBackoff::OnSuccess(ui64) {
    return EUpdate::Applied;
}

ui64 TAsyncBackoff::GetVersion() const {
    return Version;
}

TMonotonic TAsyncBackoff::GetNextAllowedAt() const {
    return NextAllowedAt;
}

} // namespace NKikimr
