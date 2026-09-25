#include "loopback_http_server.h"
#include "wait.h"

#include <util/generic/algorithm.h>
#include <util/generic/scope.h>
#include <util/generic/yexception.h>
#include <util/string/ascii.h>
#include <util/string/builder.h>
#include <util/string/cast.h>
#include <util/string/strip.h>
#include <util/system/error.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

namespace NYql::NTransportTest {

namespace {

constexpr size_t BIG_BODY_CHUNK = 64 * 1024;
constexpr size_t MAX_HEAD_SIZE = 1 << 20;
// Tick used while a handler holds a response behind a gate: it re-checks the gate state between
// polls of the socket. This is not a sleep oracle: the hold ends on an event (gate release, peer
// close or server stop).
constexpr int GATE_RECHECK_MS = 5;

TString ReasonPhrase(ui16 status) {
    switch (status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 408: return "Request Timeout";
        case 416: return "Range Not Satisfiable";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Status";
    }
}

bool IsRequestLine(TStringBuf line) {
    // METHOD SP target SP HTTP/x.y
    TStringBuf method;
    TStringBuf rest;
    if (!line.TrySplit(' ', method, rest) || method.empty()) {
        return false;
    }
    for (const char c : method) {
        if (!IsAsciiUpper(c)) {
            return false;
        }
    }
    TStringBuf target;
    TStringBuf version;
    if (!rest.TrySplit(' ', target, version) || target.empty()) {
        return false;
    }
    return version.StartsWith("HTTP/");
}

TMaybe<std::pair<ui64, ui64>> ParseRange(const TString& value, ui64 total) {
    // Only the single-range "bytes=a-b" / "bytes=a-" / "bytes=-n" forms.
    TStringBuf spec = value;
    if (!spec.SkipPrefix("bytes=")) {
        return Nothing();
    }
    TStringBuf from;
    TStringBuf to;
    if (!spec.TrySplit('-', from, to)) {
        return Nothing();
    }
    ui64 first = 0;
    ui64 last = total ? total - 1 : 0;
    if (from.empty()) {
        ui64 suffix = 0;
        if (!TryFromString(to, suffix)) {
            return Nothing();
        }
        first = suffix >= total ? 0 : total - suffix;
    } else {
        if (!TryFromString(from, first)) {
            return Nothing();
        }
        if (!to.empty() && !TryFromString(to, last)) {
            return Nothing();
        }
    }
    return std::make_pair(first, std::min(last, total ? total - 1 : 0));
}

void IgnoreSigPipeOnce() {
    // SSL_write() uses write(2), which raises SIGPIPE on a reset peer. The harness tolerates resets.
    static const bool done = []() {
        signal(SIGPIPE, SIG_IGN);
        return true;
    }();
    Y_UNUSED(done);
}

} // namespace

TMaybe<TString> TReceivedRequest::Header(TStringBuf name) const {
    for (const auto& [key, value] : Headers) {
        if (AsciiEqualsIgnoreCase(key, name)) {
            return value;
        }
    }
    return Nothing();
}

bool TReceivedRequest::HasHeader(TStringBuf name) const {
    return Header(name).Defined();
}

bool TReceivedRequest::Contains(TStringBuf needle) const {
    return RawHead.Contains(needle) || Body.Contains(needle);
}

TScriptedResponse TScriptedResponse::Ok(TString body) {
    return WithStatus(200, std::move(body));
}

TScriptedResponse TScriptedResponse::WithStatus(ui16 status, TString body) {
    TScriptedResponse response;
    response.Status = status;
    response.Body = std::move(body);
    return response;
}

TScriptedResponse TScriptedResponse::Redirect(ui16 status, TString location) {
    TScriptedResponse response = WithStatus(status);
    response.AddHeader("Location", std::move(location));
    return response;
}

TScriptedResponse TScriptedResponse::CloseAfterRequest() {
    TScriptedResponse response;
    response.CloseWithoutAnswer = true;
    return response;
}

TScriptedResponse TScriptedResponse::StallForever() {
    TScriptedResponse response;
    response.Stall = true;
    return response;
}

TScriptedResponse TScriptedResponse::BigBody(ui64 size, char fill) {
    TScriptedResponse response;
    response.BigBodySize = size;
    response.BigBodyFill = fill;
    return response;
}

TScriptedResponse TScriptedResponse::EchoRequestHeaders(ui16 status) {
    TScriptedResponse response = WithStatus(status);
    response.EchoRequestHeadersInBody = true;
    return response;
}

TScriptedResponse& TScriptedResponse::AddHeader(TString name, TString value) {
    Headers.emplace_back(std::move(name), std::move(value));
    return *this;
}

TScriptedResponse& TScriptedResponse::Gated(TString gate) {
    Gate = std::move(gate);
    return *this;
}

TScriptedResponse& TScriptedResponse::Trickle(size_t bytes, TDuration every) {
    Y_ENSURE(bytes > 0, "Trickle needs a positive piece size");
    TrickleBytes = bytes;
    TrickleEvery = every;
    return *this;
}

TScriptedResponse& TScriptedResponse::CloseMidBody(ui64 afterBytes) {
    CloseMidBodyAfter = afterBytes;
    return *this;
}

TScriptedResponse& TScriptedResponse::NoRange() {
    IgnoreRange = true;
    return *this;
}

TScriptedResponse& TScriptedResponse::KeepConnection() {
    KeepAlive = true;
    return *this;
}

struct TLoopbackHttpServer::TImpl {
    class TConnection;

    explicit TImpl(TLoopbackHttpServerOptions options);
    ~TImpl();

    void Start();
    void Stop();
    void AcceptLoop();
    void Serve(int fd, ui64 connectionId);
    bool IsStopping() const {
        return Stopping.load();
    }
    TScriptedResponse Select(const TReceivedRequest& request);
    bool GateOpen(const TString& gate) const;
    TReceivedRequest Record(TReceivedRequest request);
    void MarkClosedByPeer(ui64 index);
    bool Respond(TConnection& connection, const TReceivedRequest& request, const TScriptedResponse& response);

    const TLoopbackHttpServerOptions Options;
    int ListenFd = -1;
    int StopPipe[2] = {-1, -1};
    ui16 Port = 0;
    SSL_CTX* SslContext = nullptr;
    std::atomic<bool> Stopping = false;
    std::thread Acceptor;

    mutable std::mutex Mutex;
    TVector<std::thread> Workers;
    TVector<TReceivedRequest> Log;
    THashMap<std::pair<TString, TString>, ui32> Attempts;
    THandler Handler;
    THashMap<TString, TVector<TScriptedResponse>> Scripts;
    std::optional<TScriptedResponse> Default;
    THashSet<TString> ClosedGates;

    std::atomic<ui64> ConnectionsAccepted = 0;
    std::atomic<ui64> ConnectionsFinished = 0;
    std::atomic<ui32> ConcurrentNow = 0;
    std::atomic<ui32> MaxConcurrentSeen = 0;
    std::atomic<ui64> BlockedOnSend = 0;
    std::atomic<ui64> HandshakesOk = 0;
    std::atomic<ui64> HandshakesFailed = 0;
    std::atomic<ui64> HttpRequestsDecrypted = 0;
};

class TLoopbackHttpServer::TImpl::TConnection {
public:
    TConnection(TImpl& server, int fd)
        : Server_(server)
        , Fd_(fd)
    {
    }

    ~TConnection() {
        if (Ssl_) {
            ERR_clear_error();
            SSL_shutdown(Ssl_);
            SSL_free(Ssl_);
            ERR_clear_error();
        }
        close(Fd_);
    }

    bool PeerClosed() const {
        return PeerClosed_;
    }

    // Returns false on handshake failure or server stop.
    bool Handshake() {
        Ssl_ = SSL_new(Server_.SslContext);
        Y_ENSURE(Ssl_, "SSL_new failed");
        SSL_set_fd(Ssl_, Fd_);
        while (true) {
            ERR_clear_error();
            const int result = SSL_accept(Ssl_);
            if (result == 1) {
                return true;
            }
            const int error = SSL_get_error(Ssl_, result);
            if (error == SSL_ERROR_WANT_READ && WaitIo(POLLIN)) {
                continue;
            }
            if (error == SSL_ERROR_WANT_WRITE && WaitIo(POLLOUT)) {
                continue;
            }
            ERR_clear_error();
            return false;
        }
    }

    // > 0 bytes read; 0 = peer closed (PeerClosed() is set); -1 = server stopping.
    ssize_t ReadSome(char* buffer, size_t size) {
        while (true) {
            if (Ssl_) {
                ERR_clear_error();
                const int result = SSL_read(Ssl_, buffer, static_cast<int>(std::min<size_t>(size, 1 << 30)));
                if (result > 0) {
                    return result;
                }
                const int error = SSL_get_error(Ssl_, result);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                    if (!WaitIo(error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT)) {
                        return -1;
                    }
                    continue;
                }
                ERR_clear_error();
            } else {
                const ssize_t result = recv(Fd_, buffer, size, 0);
                if (result > 0) {
                    return result;
                }
                if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                    if (!WaitIo(POLLIN)) {
                        return -1;
                    }
                    continue;
                }
            }
            PeerClosed_ = true;
            return 0;
        }
    }

    // Appends to Buffer; false on peer close or stop.
    bool ReadMore() {
        char chunk[16 * 1024];
        const ssize_t read = ReadSome(chunk, sizeof(chunk));
        if (read <= 0) {
            return false;
        }
        Buffer_.append(chunk, read);
        return true;
    }

    bool ReadLine(TString& line) {
        while (true) {
            if (const auto pos = Buffer_.find("\r\n"); pos != TString::npos) {
                line = Buffer_.substr(0, pos);
                Buffer_.erase(0, pos + 2);
                return true;
            }
            if (!ReadMore()) {
                return false;
            }
        }
    }

    bool ReadExact(size_t size, TString& out) {
        while (Buffer_.size() < size) {
            if (!ReadMore()) {
                return false;
            }
        }
        out.append(Buffer_.data(), size);
        Buffer_.erase(0, size);
        return true;
    }

    enum class EReadResult {
        Request,
        NonHttp,
        Closed,
    };

    EReadResult ReadRequest(TReceivedRequest& request) {
        // Head.
        size_t headEnd = TString::npos;
        while ((headEnd = Buffer_.find("\r\n\r\n")) == TString::npos) {
            if (const auto lineEnd = Buffer_.find("\r\n"); lineEnd != TString::npos) {
                if (!IsRequestLine(TStringBuf(Buffer_).Head(lineEnd))) {
                    request.NonHttp = true;
                    request.RawHead = Buffer_.substr(0, lineEnd);
                    return EReadResult::NonHttp;
                }
            }
            if (Buffer_.size() > MAX_HEAD_SIZE || !ReadMore()) {
                if (Buffer_.empty()) {
                    return EReadResult::Closed;
                }
                request.NonHttp = true;
                request.RawHead = Buffer_;
                return EReadResult::NonHttp;
            }
        }
        request.RawHead = Buffer_.substr(0, headEnd);
        Buffer_.erase(0, headEnd + 4);

        TStringBuf head = request.RawHead;
        TStringBuf requestLine = head.NextTok("\r\n");
        if (!IsRequestLine(requestLine)) {
            request.NonHttp = true;
            return EReadResult::NonHttp;
        }
        request.Method = TString(requestLine.NextTok(' '));
        request.Target = TString(requestLine.NextTok(' '));
        request.Version = TString(requestLine);
        TStringBuf path;
        TStringBuf query;
        if (TStringBuf(request.Target).TrySplit('?', path, query)) {
            request.Path = TString(path);
            request.Query = TString(query);
        } else {
            request.Path = request.Target;
        }
        while (head) {
            const TStringBuf line = head.NextTok("\r\n");
            TStringBuf name;
            TStringBuf value;
            if (line.TrySplit(':', name, value)) {
                request.Headers.emplace_back(TString(StripString(name)), TString(StripString(value)));
            }
        }

        // Body.
        if (const auto expect = request.Header("Expect"); expect && AsciiEqualsIgnoreCase(*expect, "100-continue")) {
            if (!WriteAll("HTTP/1.1 100 Continue\r\n\r\n")) {
                return EReadResult::Closed;
            }
        }
        const auto transferEncoding = request.Header("Transfer-Encoding");
        if (transferEncoding && AsciiEqualsIgnoreCase(*transferEncoding, "chunked")) {
            while (true) {
                TString sizeLine;
                if (!ReadLine(sizeLine)) {
                    return EReadResult::Closed;
                }
                const size_t chunkSize = IntFromString<size_t, 16>(StripString(TStringBuf(sizeLine).Before(';')));
                if (chunkSize == 0) {
                    TString trailer;
                    do {
                        if (!ReadLine(trailer)) {
                            return EReadResult::Closed;
                        }
                    } while (!trailer.empty());
                    break;
                }
                TString crlf;
                if (!ReadExact(chunkSize, request.Body) || !ReadExact(2, crlf)) {
                    return EReadResult::Closed;
                }
            }
        } else if (const auto contentLength = request.Header("Content-Length")) {
            if (!ReadExact(FromString<size_t>(*contentLength), request.Body)) {
                return EReadResult::Closed;
            }
        }
        return EReadResult::Request;
    }

    bool WriteAll(TStringBuf data) {
        while (!data.empty()) {
            if (Server_.IsStopping()) {
                return false;
            }
            if (Ssl_) {
                ERR_clear_error();
                const int result = SSL_write(Ssl_, data.data(), static_cast<int>(std::min<size_t>(data.size(), 1 << 30)));
                if (result > 0) {
                    data.Skip(result);
                    continue;
                }
                const int error = SSL_get_error(Ssl_, result);
                if (error == SSL_ERROR_WANT_WRITE) {
                    ++Server_.BlockedOnSend;
                    if (!WaitIo(POLLOUT)) {
                        return false;
                    }
                    continue;
                }
                if (error == SSL_ERROR_WANT_READ) {
                    if (!WaitIo(POLLIN)) {
                        return false;
                    }
                    continue;
                }
                ERR_clear_error();
            } else {
                const ssize_t result = send(Fd_, data.data(), data.size(), MSG_NOSIGNAL);
                if (result > 0) {
                    data.Skip(result);
                    continue;
                }
                if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    ++Server_.BlockedOnSend;
                    if (!WaitIo(POLLOUT)) {
                        return false;
                    }
                    continue;
                }
                if (result < 0 && errno == EINTR) {
                    continue;
                }
            }
            PeerClosed_ = true;
            return false;
        }
        return true;
    }

    // Holds until `released()` (checked every GATE_RECHECK_MS when given), the peer closes, or the
    // server stops. Returns true only when released.
    bool Hold(const std::function<bool()>& released) {
        bool watchInput = true;
        while (true) {
            if (released && released()) {
                return true;
            }
            pollfd fds[2] = {
                {Fd_, static_cast<short>(POLLRDHUP | (watchInput ? POLLIN : 0)), 0},
                {Server_.StopPipe[0], POLLIN, 0},
            };
            const int ready = poll(fds, 2, released ? GATE_RECHECK_MS : -1);
            if (ready < 0 && errno != EINTR) {
                return false;
            }
            if (fds[1].revents) {
                return false;
            }
            if (fds[0].revents & (POLLRDHUP | POLLHUP | POLLERR)) {
                PeerClosed_ = true;
                return false;
            }
            if (fds[0].revents & POLLIN) {
                char byte;
                const ssize_t peeked = recv(Fd_, &byte, 1, MSG_PEEK);
                if (peeked == 0 || (peeked < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    PeerClosed_ = true;
                    return false;
                }
                // Unexpected input (pipelined request, TLS record): stop watching it, keep watching hang-up.
                watchInput = false;
            }
        }
    }

private:
    // Waits for `events` on the socket or for server stop. False on stop.
    bool WaitIo(short events) {
        while (true) {
            pollfd fds[2] = {
                {Fd_, events, 0},
                {Server_.StopPipe[0], POLLIN, 0},
            };
            const int ready = poll(fds, 2, -1);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready < 0 || fds[1].revents) {
                return false;
            }
            return true;
        }
    }

    TImpl& Server_;
    const int Fd_;
    SSL* Ssl_ = nullptr;
    TString Buffer_;
    bool PeerClosed_ = false;
};

TLoopbackHttpServer::TImpl::TImpl(TLoopbackHttpServerOptions options)
    : Options(std::move(options))
{
    IgnoreSigPipeOnce();
    if (Options.Tls) {
        SslContext = SSL_CTX_new(TLS_server_method());
        Y_ENSURE(SslContext, "SSL_CTX_new failed");
        SSL_CTX_set_mode(SslContext, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        BIO* certBio = BIO_new_mem_buf(Options.Tls->CertPem.data(), Options.Tls->CertPem.size());
        X509* cert = PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr);
        BIO_free(certBio);
        BIO* keyBio = BIO_new_mem_buf(Options.Tls->KeyPem.data(), Options.Tls->KeyPem.size());
        EVP_PKEY* key = PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr);
        BIO_free(keyBio);
        const bool loaded = cert && key
            && SSL_CTX_use_certificate(SslContext, cert) == 1
            && SSL_CTX_use_PrivateKey(SslContext, key) == 1
            && SSL_CTX_check_private_key(SslContext) == 1;
        X509_free(cert);
        EVP_PKEY_free(key);
        if (!loaded) {
            SSL_CTX_free(SslContext);
            ythrow yexception() << "cannot load the TLS certificate/key into the loopback server";
        }
    }

    Y_ENSURE(pipe2(StopPipe, O_CLOEXEC) == 0, "pipe2 failed: " << LastSystemErrorText());
    ListenFd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    Y_ENSURE(ListenFd >= 0, "socket failed: " << LastSystemErrorText());
    const int one = 1;
    setsockopt(ListenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    Y_ENSURE(inet_pton(AF_INET, Options.BindAddress.c_str(), &address.sin_addr) == 1,
        "bad IPv4 BindAddress: " << Options.BindAddress);
    address.sin_port = htons(Options.Port);
    Y_ENSURE(bind(ListenFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind failed: " << LastSystemErrorText());
    socklen_t length = sizeof(address);
    Y_ENSURE(getsockname(ListenFd, reinterpret_cast<sockaddr*>(&address), &length) == 0, "getsockname failed");
    Port = ntohs(address.sin_port);
    Y_ENSURE(listen(ListenFd, Options.ListenBacklog) == 0, "listen failed: " << LastSystemErrorText());
}

TLoopbackHttpServer::TImpl::~TImpl() {
    Stop();
    close(ListenFd);
    close(StopPipe[0]);
    close(StopPipe[1]);
    if (SslContext) {
        SSL_CTX_free(SslContext);
    }
}

void TLoopbackHttpServer::TImpl::Start() {
    Acceptor = std::thread([this]() {
        AcceptLoop();
    });
}

void TLoopbackHttpServer::TImpl::Stop() {
    if (Stopping.exchange(true)) {
        return;
    }
    const char byte = 'x';
    Y_UNUSED(write(StopPipe[1], &byte, 1));
    if (Acceptor.joinable()) {
        Acceptor.join();
    }
    TVector<std::thread> workers;
    {
        std::lock_guard lock(Mutex);
        workers.swap(Workers);
    }
    for (auto& worker : workers) {
        worker.join();
    }
}

void TLoopbackHttpServer::TImpl::AcceptLoop() {
    while (true) {
        pollfd fds[2] = {
            {ListenFd, POLLIN, 0},
            {StopPipe[0], POLLIN, 0},
        };
        const int ready = poll(fds, 2, -1);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0 || fds[1].revents) {
            return;
        }
        const int fd = accept4(ListenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        const ui64 connectionId = ConnectionsAccepted++;
        std::lock_guard lock(Mutex);
        Workers.emplace_back([this, fd, connectionId]() {
            Serve(fd, connectionId);
        });
    }
}

void TLoopbackHttpServer::TImpl::Serve(int fd, ui64 connectionId) {
    Y_DEFER {
        ++ConnectionsFinished;
    };
    TConnection connection(*this, fd);
    if (SslContext) {
        if (!connection.Handshake()) {
            if (!IsStopping()) {
                ++HandshakesFailed;
            }
            return;
        }
        ++HandshakesOk;
    }
    while (!IsStopping()) {
        TReceivedRequest request;
        const auto read = connection.ReadRequest(request);
        if (read == TConnection::EReadResult::Closed) {
            return;
        }
        request.ConnectionId = connectionId;
        request.Tls = SslContext != nullptr;
        const bool nonHttp = request.NonHttp;
        if (request.Tls && !nonHttp) {
            ++HttpRequestsDecrypted;
        }
        const TReceivedRequest recorded = Record(std::move(request));
        const ui64 index = recorded.Index;
        if (nonHttp) {
            return;
        }
        const TScriptedResponse response = Select(recorded);

        const ui32 now = ++ConcurrentNow;
        ui32 seen = MaxConcurrentSeen.load();
        while (now > seen && !MaxConcurrentSeen.compare_exchange_weak(seen, now)) {
        }
        const bool keepAlive = Respond(connection, recorded, response);
        --ConcurrentNow;
        if (connection.PeerClosed()) {
            MarkClosedByPeer(index);
        }
        if (!keepAlive) {
            return;
        }
    }
}

TReceivedRequest TLoopbackHttpServer::TImpl::Record(TReceivedRequest request) {
    std::lock_guard lock(Mutex);
    request.Index = Log.size();
    request.Arrived = TInstant::Now();
    if (!request.NonHttp) {
        request.Attempt = Attempts[std::make_pair(request.Method, request.Path)]++;
    }
    Log.push_back(std::move(request));
    return Log.back();
}

void TLoopbackHttpServer::TImpl::MarkClosedByPeer(ui64 index) {
    std::lock_guard lock(Mutex);
    Log[index].ConnectionClosedByPeer = true;
}

TScriptedResponse TLoopbackHttpServer::TImpl::Select(const TReceivedRequest& request) {
    THandler handler;
    {
        std::lock_guard lock(Mutex);
        handler = Handler;
        if (!handler) {
            if (const auto it = Scripts.find(request.Path); it != Scripts.end() && !it->second.empty()) {
                const auto& perAttempt = it->second;
                return perAttempt[std::min<size_t>(request.Attempt, perAttempt.size() - 1)];
            }
            if (Default) {
                return *Default;
            }
            return TScriptedResponse::WithStatus(404, "unscripted");
        }
    }
    return handler(request);
}

bool TLoopbackHttpServer::TImpl::GateOpen(const TString& gate) const {
    if (gate.empty()) {
        return true;
    }
    std::lock_guard lock(Mutex);
    return !ClosedGates.contains(gate);
}

bool TLoopbackHttpServer::TImpl::Respond(TConnection& connection, const TReceivedRequest& request, const TScriptedResponse& response) {
    if (response.Stall) {
        connection.Hold({});
        return false;
    }
    if (response.CloseWithoutAnswer) {
        return false;
    }
    const auto released = [&]() {
        return GateOpen(request.Path) && GateOpen(response.Gate);
    };
    if (!released() && !connection.Hold(released)) {
        return false;
    }

    ui16 status = response.Status;
    TString body = response.EchoRequestHeadersInBody ? request.RawHead : response.Body;
    THttpHeaderList headers = response.Headers;
    const bool bigBody = response.BigBodySize.has_value();
    ui64 bodySize = bigBody ? *response.BigBodySize : body.size();

    if (status == 200 && !response.IgnoreRange && !bigBody) {
        if (const auto range = request.Header("Range")) {
            if (const auto parsed = ParseRange(*range, body.size())) {
                if (parsed->first >= body.size()) {
                    status = 416;
                    headers.emplace_back("Content-Range", TStringBuilder() << "bytes */" << body.size());
                    body.clear();
                } else {
                    status = 206;
                    headers.emplace_back("Content-Range", TStringBuilder() << "bytes " << parsed->first << "-" << parsed->second << "/" << body.size());
                    body = body.substr(parsed->first, parsed->second - parsed->first + 1);
                }
                bodySize = body.size();
            }
        }
    }

    const auto hasHeader = [&](TStringBuf name) {
        return AnyOf(headers, [&](const auto& header) {
            return AsciiEqualsIgnoreCase(header.first, name);
        });
    };
    TStringBuilder head;
    head << "HTTP/1.1 " << status << " " << (response.Reason ? response.Reason : ReasonPhrase(status)) << "\r\n";
    for (const auto& [name, value] : headers) {
        head << name << ": " << value << "\r\n";
    }
    if (!hasHeader("Content-Length") && !hasHeader("Transfer-Encoding")) {
        head << "Content-Length: " << bodySize << "\r\n";
    }
    if (!hasHeader("Connection")) {
        head << "Connection: " << (response.KeepAlive ? "keep-alive" : "close") << "\r\n";
    }
    head << "\r\n";
    if (!connection.WriteAll(head)) {
        return false;
    }

    const ui64 sendLimit = response.CloseMidBodyAfter ? std::min(*response.CloseMidBodyAfter, bodySize) : bodySize;
    const auto sendPiece = [&](TStringBuf piece) {
        if (!response.TrickleBytes) {
            return connection.WriteAll(piece);
        }
        while (!piece.empty()) {
            if (!connection.WriteAll(piece.NextTokAt(std::min(piece.size(), response.TrickleBytes)))) {
                return false;
            }
            if (!piece.empty() && response.TrickleEvery) {
                // Server-side stimulus (Trickle mode), not a test oracle.
                std::this_thread::sleep_for(std::chrono::microseconds(response.TrickleEvery.MicroSeconds()));
            }
        }
        return true;
    };
    if (bigBody) {
        const TString chunk(BIG_BODY_CHUNK, response.BigBodyFill);
        for (ui64 sent = 0; sent < sendLimit;) {
            const size_t piece = std::min<ui64>(chunk.size(), sendLimit - sent);
            if (!sendPiece(TStringBuf(chunk).Head(piece))) {
                return false;
            }
            sent += piece;
        }
    } else if (!sendPiece(TStringBuf(body).Head(sendLimit))) {
        return false;
    }
    if (response.CloseMidBodyAfter) {
        return false;
    }
    return response.KeepAlive;
}

TLoopbackHttpServer::TLoopbackHttpServer(TLoopbackHttpServerOptions options)
    : Impl_(std::make_unique<TImpl>(std::move(options)))
{
    Impl_->Start();
}

TLoopbackHttpServer::~TLoopbackHttpServer() {
    Impl_->Stop();
}

ui16 TLoopbackHttpServer::Port() const {
    return Impl_->Port;
}

bool TLoopbackHttpServer::IsTls() const {
    return Impl_->SslContext != nullptr;
}

TString TLoopbackHttpServer::Url(TStringBuf path, TStringBuf host) const {
    return TStringBuilder() << (IsTls() ? "https://" : "http://") << host << ":" << Port() << path;
}

void TLoopbackHttpServer::SetHandler(THandler handler) {
    std::lock_guard lock(Impl_->Mutex);
    Impl_->Handler = std::move(handler);
}

void TLoopbackHttpServer::Script(TString path, TVector<TScriptedResponse> perAttempt) {
    Y_ENSURE(!perAttempt.empty(), "Script needs at least one response");
    std::lock_guard lock(Impl_->Mutex);
    Impl_->Scripts[std::move(path)] = std::move(perAttempt);
}

void TLoopbackHttpServer::SetDefault(TScriptedResponse response) {
    std::lock_guard lock(Impl_->Mutex);
    Impl_->Default = std::move(response);
}

void TLoopbackHttpServer::Gate(TString gate) {
    std::lock_guard lock(Impl_->Mutex);
    Impl_->ClosedGates.insert(std::move(gate));
}

void TLoopbackHttpServer::Release(const TString& gate) {
    std::lock_guard lock(Impl_->Mutex);
    Impl_->ClosedGates.erase(gate);
}

TVector<TReceivedRequest> TLoopbackHttpServer::Requests() const {
    std::lock_guard lock(Impl_->Mutex);
    return Impl_->Log;
}

size_t TLoopbackHttpServer::RequestCount() const {
    std::lock_guard lock(Impl_->Mutex);
    return Impl_->Log.size();
}

ui32 TLoopbackHttpServer::MaxConcurrent() const {
    return Impl_->MaxConcurrentSeen.load();
}

ui32 TLoopbackHttpServer::Concurrent() const {
    return Impl_->ConcurrentNow.load();
}

ui64 TLoopbackHttpServer::ConnectionsAccepted() const {
    return Impl_->ConnectionsAccepted.load();
}

ui64 TLoopbackHttpServer::ConnectionsFinished() const {
    return Impl_->ConnectionsFinished.load();
}

void TLoopbackHttpServer::WaitConnectionsDone(ui64 count, TDuration guard) const {
    NTransportTest::WaitUntil([&]() {
        const ui64 accepted = ConnectionsAccepted();
        return accepted >= count && ConnectionsFinished() == accepted;
    }, guard, TStringBuilder() << "at least " << count << " connection(s) accepted and all of them finished");
}

ui64 TLoopbackHttpServer::BlockedOnSendCount() const {
    return Impl_->BlockedOnSend.load();
}

bool TLoopbackHttpServer::AnyRequestContains(TStringBuf needle) const {
    std::lock_guard lock(Impl_->Mutex);
    return AnyOf(Impl_->Log, [&](const TReceivedRequest& request) {
        return request.Contains(needle);
    });
}

ui64 TLoopbackHttpServer::HandshakesOk() const {
    return Impl_->HandshakesOk.load();
}

ui64 TLoopbackHttpServer::HandshakesFailed() const {
    return Impl_->HandshakesFailed.load();
}

ui64 TLoopbackHttpServer::HttpRequestsDecrypted() const {
    return Impl_->HttpRequestsDecrypted.load();
}

void TLoopbackHttpServer::WaitForRequests(size_t count, TDuration guard) const {
    NTransportTest::WaitUntil([&]() {
        return RequestCount() >= count;
    }, guard, TStringBuilder() << count << " request(s) at the loopback server");
}

void TLoopbackHttpServer::WaitUntil(const std::function<bool()>& predicate, TDuration guard, TStringBuf what) const {
    NTransportTest::WaitUntil(predicate, guard, what);
}

void TLoopbackHttpServer::WaitBlockedOnSend(ui64 count, TDuration guard) const {
    NTransportTest::WaitUntil([&]() {
        return BlockedOnSendCount() >= count;
    }, guard, TStringBuilder() << "BlockedOnSend >= " << count);
}

void TLoopbackHttpServer::WaitConnectionClosedByPeer(ui64 requestIndex, TDuration guard) const {
    NTransportTest::WaitUntil([&]() {
        std::lock_guard lock(Impl_->Mutex);
        return requestIndex < Impl_->Log.size() && Impl_->Log[requestIndex].ConnectionClosedByPeer;
    }, guard, TStringBuilder() << "peer closes the connection of request #" << requestIndex);
}

void TLoopbackHttpServer::Stop() {
    Impl_->Stop();
}

} // namespace NYql::NTransportTest
