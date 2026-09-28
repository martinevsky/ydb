#pragma once

// TRefusingPort: a loopback port where a connect() is refused (ECONNREFUSED; curl: CURLE_COULDNT_CONNECT).
//
// The port stays bound to 127.0.0.1 (without listen()) for the lifetime of the object, so no other
// process can take it and start accepting. A port that was bound and then closed gives no such
// guarantee: the kernel can hand it out again at once.

#include <util/generic/string.h>
#include <util/generic/strbuf.h>
#include <util/network/socket.h>

namespace NYql::NTransportTest {

class TRefusingPort {
public:
    // Throws yexception if no port could be bound.
    TRefusingPort();

    TRefusingPort(const TRefusingPort&) = delete;
    TRefusingPort& operator=(const TRefusingPort&) = delete;

    ui16 Port() const {
        return Port_;
    }
    // "http://127.0.0.1:<port><path>".
    TString Url(TStringBuf path = "/obj") const;

private:
    TSocketHolder Socket_;
    ui16 Port_ = 0;
};

} // namespace NYql::NTransportTest
