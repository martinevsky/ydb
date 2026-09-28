# Transport test harness

Shared helpers for the FQ transport test plan (curl HTTP gateway, Solomon accessor, Solomon sink).
Component IDs (H1..H12) follow `test_plan.md` §3. Validation tests live in
`ydb/library/yql/providers/common/http_gateway/yql_http_gateway_transport_ut.cpp`
(suites `THttpGatewayTransportTest` and `TTransportHarnessTest`); read them for worked examples.

Rules for every user of this harness:

- **Framework-neutral.** Nothing here uses `UNIT_ASSERT` or gtest. Helpers return values or throw
  `yexception`; a thrown exception fails the test in UNITTEST and GTEST alike.
- **Guards are failure deadlines, never oracles.** Every wait takes a guard and throws when it expires.
  No `Sleep` anywhere, except the H1 Trickle stimulus inside the server thread.
- **Singleton hygiene.** Make the curl gateway only through `TGatewayScope` (H11). Do not link
  `http_gateway/ut_helpers` (`THttpGatewayHolder`) into the same target.

## Libraries (PEERDIR)

| PEERDIR | Contents | Depends on |
|---|---|---|
| `ydb/library/yql/providers/common/ut_helpers/transport` | known_bug.h, H1, H2, H6, H8, H12, wait.h | util, `library/cpp/monlib/dynamic_counters`, `contrib/libs/openssl` |
| `.../ut_helpers/transport/yql` | H7 `TLogCapture`, H11 `TGatewayScope` | the curl gateway, the YQL logger |
| `.../ut_helpers/transport/credentials` | H10 `TFakeCredentialsProvider` | YDB C++ SDK credentials |
| `ydb/library/yql/providers/common/http_gateway/mock` | H9 `TScriptedHttpGateway` (`yql_http_scripted_gateway.h`) | the curl gateway interface |

Namespace: `NYql::NTransportTest` (H9 is `NYql::TScriptedHttpGateway`).

## Known-bug guard (`known_bug.h`, `known_bug_gtest.h`)

A contract test that fails on today's code starts with the guard. It is skipped (the body returns and
`SKIP known bug <id>` goes to stderr) unless `YDB_TRANSPORT_RUN_KNOWN_BUGS=1`:

```cpp
Y_UNIT_TEST(SelfSignedServerRejectedByDefault) {
    YDB_SKIP_KNOWN_BUG("F-A-1");            // UNITTEST
    ...
}
TEST(TSuite, Test) {
    YDB_SKIP_KNOWN_BUG_GTEST("F-B-5");      // GTEST: include known_bug_gtest.h (wraps GTEST_SKIP())
    ...
}
```

Show it red: `./ya make --build relwithdebinfo -tA <target> -F 'Suite::Test' --test-env=YDB_TRANSPORT_RUN_KNOWN_BUGS=1`.
Decision-gated contract variants use the id `DG-<test id>`.

## Components

### H1 `TLoopbackHttpServer` (`loopback_http_server.h`)
Raw HTTP/1.1 on `127.0.0.1:<port 0>`, one thread per connection, plain or TLS.
Options: `.Tls`, `.BindAddress` (any 127.0.0.0/8 address, default `127.0.0.1`), `.Port` (default 0 =
ephemeral). Two servers can share a port on different loopback addresses (DNS routing tests):
`TLoopbackHttpServer second({.BindAddress = "127.0.0.2", .Port = first.Port()});`

```cpp
TLoopbackHttpServer server;                                  // or ({.Tls = pki.Leaf()})
server.Script("/obj", {TScriptedResponse::WithStatus(503), TScriptedResponse::Ok("abc")}); // per attempt, last repeats
server.SetDefault(TScriptedResponse::Ok("x"));               // fallback; unscripted = 404
server.SetHandler([](const TReceivedRequest& r) { ... });    // overrides scripts
TString url = server.Url("/obj");                            // http(s)://127.0.0.1:<port>/obj, also for TLS
```

Responses (`TScriptedResponse`): `Ok(body)`, `WithStatus(code, body)`, `Redirect(code, location)`,
`StallForever()` (read the request, never answer), `CloseAfterRequest()` (read the request, close without
answering: curl gets `CURLE_GOT_NOTHING`), `BigBody(bytes)` (generated body; counts
`BlockedOnSendCount()` whenever the socket buffer is full), `EchoRequestHeaders(code)`; modifiers
`.AddHeader()`, `.Gated(name)`, `.Trickle(bytes, every)`, `.CloseMidBody(afterBytes)`, `.NoRange()` (a 200
answers a `Range` request with the full body; by default it becomes 206 with the slice), `.KeepConnection()`
(default: `Connection: close`, so every request is a new connection and a new TLS handshake).

Gates: `server.Gate("/path")` holds every response for that path (or for `.Gated("/path")`) until
`server.Release("/path")`.

Observables: `Requests()` (snapshot of `TReceivedRequest`: method, path, query, headers, body, raw head,
`Attempt`, `ConnectionId`, `NonHttp`, `ConnectionClosedByPeer`; `Header(name)` is case-insensitive),
`RequestCount()`, `AnyRequestContains(secret)`, `Concurrent()`, `MaxConcurrent()`, `ConnectionsAccepted()`,
`ConnectionsFinished()`, `BlockedOnSendCount()`. TLS: `HandshakesOk()`, `HandshakesFailed()`,
`HttpRequestsDecrypted()`.

Sync points (all throw on guard): `WaitForRequests(n)`, `WaitBlockedOnSend(n)`,
`WaitConnectionClosedByPeer(requestIndex)`, `WaitConnectionsDone(n)` (use it before asserting that
nothing was delivered: the server's view of every connection is final), `WaitUntil(pred, guard, what)`.

Non-HTTP input (e.g. a gopher selector) is logged as one request with `NonHttp = true` and closed.
The server ignores `SIGPIPE` process-wide (TLS writes use `write(2)`).

### H2 `TTestPki` (`test_pki.h`)
Generated at construction with OpenSSL (EC P-256), PEM files in a private temp dir:
`Ca()`/`CaFile()`, `Leaf()` (SAN localhost + 127.0.0.1), `WrongHostLeaf()` (SAN wrong.test),
`SelfSignedLeaf()`, `ExpiredLeaf()` (expired yesterday, signed by `Ca()`), `UntrustedCa()`/`UntrustedLeaf()`.
Each `TTestCert` has `CertPem`, `KeyPem`, `CertFile`, `KeyFile`. Pass a cert as `{.Tls = pki.Leaf()}` to H1.
Note for curl: a chain failure aborts the handshake (`HandshakesFailed`), a host name mismatch is detected
after it (`HandshakesOk` then close, `HttpRequestsDecrypted == 0`).

### H6 `TCountersInspector` (`counters_inspector.h`)
`Get("method=GET/code=200/count")`, `Find(path)` (Nothing if absent; never creates counters), `Has`,
`Snapshot()`, `Diff(before, after)`, `WaitValue(path, v)`, `AssertSettled()` (T-OBS-3: `InFlight`,
`InFlightStreams`, `AllocatedMemory`, `AwaitQueue` reach 0 within the guard).

### H7 `TLogCapture` (`yql/log_capture.h`)
Owns the YQL logger at TRACE for its lifetime (throws if another `YqlLoggerScope` is active; create it
before the code under test). `Text()`, `Lines()`, `Contains()`, `CountLines(ecmascriptRegex)`,
`AssertNoSecret(secret)`. For actor-runtime targets use `runtime.SetLogBackend` instead.

### H8 `TManualClock` (`manual_clock.h`)
`Now()`, `Advance(d)`, `Set(t)`, `AsFunction()` for `std::function<TInstant()>` seams (S1, S2).

### H9 `TScriptedHttpGateway` (`http_gateway/mock/yql_http_scripted_gateway.h`)
`IHTTPGateway` fake. `SetBufferedScript(call -> optional<TResult>)`: a result completes inline,
`std::nullopt` leaves the call pending for `Complete(callId, result)` from any thread (no script: all
pending). `SetStreamScript(stream -> void)`: drive `TStream::Start/Data/Finish` now or later on any thread;
the cancel hook delivers `OnFinish(CURLE_OK, {issue})` once and drops later data (like the real gateway).
Records `Calls()` (method, url, headers, body, offset, size limit, per-url `Attempt`, work scope),
`Cancels()`, `PoolCapsUpdates()`, `Streams()`, `PendingCalls()`, `BufferedStreamBytes()`;
`WaitForCalls(n)`. Create with `TScriptedHttpGateway::Make()`.

### H10 `TFakeCredentialsProvider` (`credentials/fake_credentials.h`)
`NYdb::ICredentialsProvider` returning a token; `FailNext(n, msg)` (default "IAM-token not ready yet"),
`Block()`/`Unblock()`, `WaitForBlockedCalls(n)`, `Calls()`.

### H11 `TGatewayScope` (`yql/gateway_scope.h`)
`TGatewayScope gateway(config);` makes the gateway with fresh counters and throws if `IHTTPGateway::Make`
returned an existing singleton. `gateway->Download(...)`, `gateway.Counters()`, `gateway.Inspector()`.
End every test with `gateway.Inspector().AssertSettled(); gateway.Close();` (`Close()` throws if anything
still holds the gateway).
`TGatewayScope gateway(config, factory);` makes the gateway with `factory(&config, counters)` instead of
`IHTTPGateway::Make`, e.g. `NHttpGatewayTest::MakeHttpGatewayForTest` with test options (below).

### H12 `TBlackholeListener` (`blackhole.h`, Linux only)
Loopback listener with a saturated accept queue: `connect()` to `Url()` hangs until the client's connect
timeout (replaces the non-routable IP trick).

## Gateway test hooks (seams S1, S2, S3, S4, S11)

`http_gateway/yql_http_gateway_test_hooks.h`, namespace `NYql::NHttpGatewayTest`. Empty options = today's
behaviour.

- `MakeHttpGatewayForTest(cfg, counters, TGatewayTestOptions{...})`: same singleton path as `Make`.
  - `.Now` (S1): clock for SigV4 signing (`x-amz-date`) and for the retry `Delayed` queue (deadline and
    due check). With H8 a retry never becomes due until the test advances the clock.
  - `.DnsResolve` (S11): resolver for `DnsResolverConfig.ExplicitDNSRecord` hosts (throw
    `TNetworkResolutionError` to fail).
  - `.CurlMulti` (S3): wraps `curl_multi_perform` / `curl_multi_poll` of the worker loop
    (`ECurlMultiCall::Perform` / `Poll`); return another `CURLMcode` to inject a CURLM failure.
- `RefreshDnsNow(gateway)` (S11): re-resolve the explicit records now.
- `GetFqHTTPRetryPolicy(clock)` (S2, `yql_http_default_retry_policy.h`): the FQ dns retry budget on a clock.
- `IHTTPGateway::GetEffectiveConfig()` (S4): the config in force; a later `Make` with a different non-null
  config logs one WARN "HTTP gateway is already created ... effective config: {...} ignored config: {...}".

Shared test helpers for target A (`TBufferedCall`, `TStreamConsumer`, `WaitPerformCycles`,
`ClosedLoopbackPort`) are in `http_gateway/yql_http_gateway_ut_common.h`.

## Gateway TLS knobs (seam S6)

`THttpGatewayConfig.CaFile` / `VerifyPeer` (`gateways_config.proto`). Unset = legacy behaviour
(`CURLOPT_SSL_VERIFYPEER=0`). `CaFile` set = verify chain and host name against that CA bundle.
`VerifyPeer=true` = verify against `CaFile` or the default store; `VerifyPeer=false` = explicit opt-out.
