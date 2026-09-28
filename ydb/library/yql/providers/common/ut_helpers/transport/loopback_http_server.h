#pragma once

// H1: TLoopbackHttpServer, a raw HTTP/1.1 server on 127.0.0.1:<port 0> (address and port configurable), one thread per connection,
// optionally speaking TLS (H2 mode). It exists to script transport faults for the curl gateway,
// the Solomon accessor and friends. Framework-neutral: failures are reported by throwing yexception.
//
// Response selection for each parsed request, first match wins:
//   1. SetHandler(handler)              -> handler(request)
//   2. Script(path, {r0, r1, ...})      -> r<attempt>, where attempt counts requests with the same
//                                          method and path (0-based); the last response repeats
//   3. SetDefault(response)             -> response
//   4. otherwise                        -> 404 "unscripted"
//
// Every connection is closed after one response unless the response has KeepAlive = true, so
// curl opens a new connection (and, in TLS mode, performs a new handshake) per request.

#include "test_pki.h"

#include <util/datetime/base.h>
#include <util/generic/hash.h>
#include <util/generic/hash_set.h>
#include <util/generic/maybe.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace NYql::NTransportTest {

using THttpHeaderList = TVector<std::pair<TString, TString>>;

struct TReceivedRequest {
    ui64 Index = 0;          // arrival order, 0-based
    ui64 ConnectionId = 0;   // accepted connections are numbered from 0
    ui32 Attempt = 0;        // requests before this one with the same Method and Path
    TString Method;
    TString Target;          // as sent: path + '?' + query
    TString Path;
    TString Query;
    TString Version;
    THttpHeaderList Headers; // in wire order, names as sent
    TString Body;            // decoded (Content-Length or chunked)
    TString RawHead;         // request line + headers, without the final CRLFCRLF
    TInstant Arrived;
    bool Tls = false;
    // The first line was not an HTTP request line (e.g. a gopher/dict selector). Only RawHead is set.
    bool NonHttp = false;
    // Set when the server saw the peer close or reset the connection while it was stalling, gated,
    // or sending the response.
    bool ConnectionClosedByPeer = false;

    // Case-insensitive lookup of the first header with this name.
    TMaybe<TString> Header(TStringBuf name) const;
    bool HasHeader(TStringBuf name) const;
    // True if RawHead or Body contains `needle`.
    bool Contains(TStringBuf needle) const;
};

struct TScriptedResponse {
    ui16 Status = 200;
    TString Reason;                 // empty = standard reason phrase
    THttpHeaderList Headers;        // Content-Length / Connection are added unless present
    TString Body;

    // Accept and read the request, never answer. Held until the peer closes or the server stops.
    bool Stall = false;
    // Read the request, then close the connection without sending anything (curl: CURLE_GOT_NOTHING).
    bool CloseWithoutAnswer = false;
    // Hold the response until Release(Gate). Gates are also keyed by request path, see Gate().
    TString Gate;
    // Send the body in pieces of TrickleBytes, pausing TrickleEvery between them (the only sleep in
    // the harness: a server-side stimulus, never an oracle).
    size_t TrickleBytes = 0;
    TDuration TrickleEvery;
    // Stream a generated body of BigBodySize bytes of BigBodyFill instead of Body. Every time the
    // kernel send buffer is full (EAGAIN / SSL_ERROR_WANT_WRITE) BlockedOnSendCount() increments.
    std::optional<ui64> BigBodySize;
    char BigBodyFill = 'x';
    // Announce the full Content-Length, send only the first CloseMidBodyAfter body bytes, then close.
    std::optional<ui64> CloseMidBodyAfter;
    // By default a 200 answer to a request with "Range: bytes=a-b" becomes 206 with that slice of Body.
    // IgnoreRange answers 200 with the full body instead.
    bool IgnoreRange = false;
    // Replace Body with the request's RawHead.
    bool EchoRequestHeadersInBody = false;
    // Keep the connection open for the next request.
    bool KeepAlive = false;

    static TScriptedResponse Ok(TString body = {});
    static TScriptedResponse WithStatus(ui16 status, TString body = {});
    static TScriptedResponse Redirect(ui16 status, TString location);
    static TScriptedResponse StallForever();
    static TScriptedResponse CloseAfterRequest();
    static TScriptedResponse BigBody(ui64 size, char fill = 'x');
    static TScriptedResponse EchoRequestHeaders(ui16 status = 200);

    TScriptedResponse& AddHeader(TString name, TString value);
    TScriptedResponse& Gated(TString gate);
    TScriptedResponse& Trickle(size_t bytes, TDuration every);
    TScriptedResponse& CloseMidBody(ui64 afterBytes);
    TScriptedResponse& NoRange();
    TScriptedResponse& KeepConnection();
};

struct TLoopbackHttpServerOptions {
    // TLS mode: the server presents this certificate (CertPem + KeyPem are used; files are ignored).
    std::optional<TTestCert> Tls;
    int ListenBacklog = 128;
    // IPv4 loopback address to listen on (any of 127.0.0.0/8 works on Linux), e.g. "127.0.0.2" for a
    // second server that a DNS test routes to.
    TString BindAddress = "127.0.0.1";
    // 0 = an ephemeral port. A fixed port lets two servers on different BindAddress share one port.
    ui16 Port = 0;
};

class TLoopbackHttpServer {
public:
    using THandler = std::function<TScriptedResponse(const TReceivedRequest&)>;

    explicit TLoopbackHttpServer(TLoopbackHttpServerOptions options = {});
    ~TLoopbackHttpServer();

    TLoopbackHttpServer(const TLoopbackHttpServer&) = delete;
    TLoopbackHttpServer& operator=(const TLoopbackHttpServer&) = delete;

    ui16 Port() const;
    bool IsTls() const;
    // "http(s)://<host>:<port><path>". The default host is 127.0.0.1 regardless of BindAddress. TLS tests
    // use it too: the TTestPki leaves carry IP:127.0.0.1 in their SAN. Do not use "localhost": the server
    // listens on IPv4 only and localhost may resolve to ::1 first, where another process on the host can
    // listen on the same port number.
    TString Url(TStringBuf path = "/obj", TStringBuf host = "127.0.0.1") const;

    // Scripting (may be changed while running; affects requests parsed afterwards).
    void SetHandler(THandler handler);
    void Script(TString path, TVector<TScriptedResponse> perAttempt);
    void SetDefault(TScriptedResponse response);

    // Gates: a closed gate holds every response whose request path equals `gate` or whose
    // TScriptedResponse::Gate equals `gate`. Release opens it for good (until Gate() again).
    void Gate(TString gate);
    void Release(const TString& gate);

    // Observables. Requests() returns a snapshot copy.
    TVector<TReceivedRequest> Requests() const;
    size_t RequestCount() const;
    ui32 MaxConcurrent() const;   // max number of requests being answered at once
    ui32 Concurrent() const;
    ui64 ConnectionsAccepted() const;
    // Connections whose server thread has finished (closed by either side). The connection's
    // log entries and TLS counters are final once it is counted here.
    ui64 ConnectionsFinished() const;
    ui64 BlockedOnSendCount() const;
    // True if any received head or body contains `needle` (e.g. a secret that must not leak).
    bool AnyRequestContains(TStringBuf needle) const;

    // TLS observables (always 0 in plain mode).
    ui64 HandshakesOk() const;
    ui64 HandshakesFailed() const;
    ui64 HttpRequestsDecrypted() const;

    // Guarded waits (throw yexception on guard expiry); implemented with NTransportTest::WaitUntil.
    void WaitForRequests(size_t count, TDuration guard = TDuration::Seconds(10)) const;
    void WaitUntil(const std::function<bool()>& predicate, TDuration guard, TStringBuf what) const;
    void WaitBlockedOnSend(ui64 count = 1, TDuration guard = TDuration::Seconds(10)) const;
    void WaitConnectionClosedByPeer(ui64 requestIndex, TDuration guard = TDuration::Seconds(10)) const;
    // Causal sync point for "nothing was delivered" oracles: at least `count` connections were accepted
    // and every accepted connection has finished.
    void WaitConnectionsDone(ui64 count = 1, TDuration guard = TDuration::Seconds(10)) const;

    // Stops accepting, wakes all gated/stalled handlers and joins every thread. Idempotent.
    void Stop();

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl_;
};

} // namespace NYql::NTransportTest
