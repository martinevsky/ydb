#pragma once

// H12: TBlackholeListener, a connect-timeout trigger (Linux only).
//
// A loopback listener with listen(fd, 0) that is never accepted. Its accept queue is pre-filled by
// client connects owned by this object, so the kernel drops further SYNs (accept-queue overflow) and a
// client connect() to Port() hangs until the client's own connect timeout fires.
// It replaces the non-routable-IP trick (RJ-7), which depends on the environment.

#include <util/generic/string.h>
#include <util/generic/strbuf.h>
#include <util/generic/vector.h>

namespace NYql::NTransportTest {

class TBlackholeListener {
public:
    // Throws yexception if the accept queue could not be saturated.
    TBlackholeListener();
    ~TBlackholeListener();

    TBlackholeListener(const TBlackholeListener&) = delete;
    TBlackholeListener& operator=(const TBlackholeListener&) = delete;

    ui16 Port() const {
        return Port_;
    }
    TString Url(TStringBuf path = "/obj") const;

private:
    int ListenFd_ = -1;
    ui16 Port_ = 0;
    TVector<int> Fillers_;
};

} // namespace NYql::NTransportTest
