#include "blackhole.h"
#include "wait.h"

#include <util/generic/yexception.h>
#include <util/string/builder.h>
#include <util/system/error.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace NYql::NTransportTest {

namespace {

// For a listening socket Linux reports the accept-queue length in tcpi_unacked and the backlog in
// tcpi_sacked. The kernel drops SYNs once length > backlog (sk_acceptq_is_full).
bool AcceptQueueFull(int listenFd) {
    tcp_info info{};
    socklen_t length = sizeof(info);
    Y_ENSURE(getsockopt(listenFd, IPPROTO_TCP, TCP_INFO, &info, &length) == 0, "TCP_INFO failed: " << LastSystemErrorText());
    return info.tcpi_unacked > info.tcpi_sacked;
}

bool Established(int fd) {
    pollfd pfd{fd, POLLOUT, 0};
    if (poll(&pfd, 1, 0) != 1) {
        return false;
    }
    int error = 0;
    socklen_t length = sizeof(error);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length);
    return error == 0;
}

} // namespace

TBlackholeListener::TBlackholeListener() {
    ListenFd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    Y_ENSURE(ListenFd_ >= 0, "socket failed: " << LastSystemErrorText());
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Y_ENSURE(bind(ListenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind failed: " << LastSystemErrorText());
    socklen_t length = sizeof(address);
    Y_ENSURE(getsockname(ListenFd_, reinterpret_cast<sockaddr*>(&address), &length) == 0, "getsockname failed");
    Port_ = ntohs(address.sin_port);
    Y_ENSURE(listen(ListenFd_, 0) == 0, "listen failed: " << LastSystemErrorText());

    for (int attempt = 0; attempt < 8 && !AcceptQueueFull(ListenFd_); ++attempt) {
        const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        Y_ENSURE(fd >= 0, "socket failed: " << LastSystemErrorText());
        Fillers_.push_back(fd);
        const int result = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        Y_ENSURE(result == 0 || errno == EINPROGRESS, "filler connect failed: " << LastSystemErrorText());
        WaitUntil([&]() {
            return Established(fd) || AcceptQueueFull(ListenFd_);
        }, TDuration::Seconds(5), "blackhole filler connection is queued");
    }
    Y_ENSURE(AcceptQueueFull(ListenFd_), "could not saturate the accept queue of the blackhole listener");
}

TBlackholeListener::~TBlackholeListener() {
    for (const int fd : Fillers_) {
        close(fd);
    }
    close(ListenFd_);
}

TString TBlackholeListener::Url(TStringBuf path) const {
    return TStringBuilder() << "http://127.0.0.1:" << Port_ << path;
}

} // namespace NYql::NTransportTest
