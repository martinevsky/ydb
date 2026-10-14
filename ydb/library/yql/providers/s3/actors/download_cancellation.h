#pragma once

#include <ydb/library/yql/providers/common/http_gateway/yql_http_gateway.h>

#include <mutex>
#include <utility>

namespace NYql::NDq {

// The gateway may deliver decoder callbacks before returning the download hook.
class TDownloadCancellation {
public:
    void Publish(IHTTPGateway::TCancelHook hook) {
        IHTTPGateway::TCancelHook previous;
        bool cancelled;
        {
            std::lock_guard guard(Mutex);
            cancelled = Cancelled;
            if (!cancelled) {
                previous = std::exchange(Hook, std::move(hook));
            }
        }
        // A previous attempt is already complete. Release its captures outside
        // the lock too, since their destructors may call back into the owner.
        if (cancelled && hook) {
            hook(TIssue("Request cancelled."));
        }
    }

    void Cancel() {
        IHTTPGateway::TCancelHook hook;
        {
            std::lock_guard guard(Mutex);
            Cancelled = true;
            hook = std::exchange(Hook, {});
        }
        if (hook) {
            hook(TIssue("Request cancelled."));
        }
    }

    bool IsCancelled() const {
        std::lock_guard guard(Mutex);
        return Cancelled;
    }

private:
    mutable std::mutex Mutex;
    bool Cancelled = false;
    IHTTPGateway::TCancelHook Hook;
};

} // namespace NYql::NDq
