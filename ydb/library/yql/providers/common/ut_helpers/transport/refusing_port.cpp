#include "refusing_port.h"

#include <util/generic/yexception.h>
#include <util/string/builder.h>
#include <util/system/error.h>

namespace NYql::NTransportTest {

TRefusingPort::TRefusingPort()
    : Socket_(::socket(AF_INET, SOCK_STREAM, 0))
{
    Y_ENSURE(!Socket_.Closed(), "socket() failed: " << LastSystemErrorText());
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    Y_ENSURE(::bind(Socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
        "bind() failed: " << LastSystemErrorText());
    socklen_t length = sizeof(address);
    Y_ENSURE(::getsockname(Socket_, reinterpret_cast<sockaddr*>(&address), &length) == 0,
        "getsockname() failed: " << LastSystemErrorText());
    Port_ = ntohs(address.sin_port);
}

TString TRefusingPort::Url(TStringBuf path) const {
    return TStringBuilder() << "http://127.0.0.1:" << Port_ << path;
}

} // namespace NYql::NTransportTest
