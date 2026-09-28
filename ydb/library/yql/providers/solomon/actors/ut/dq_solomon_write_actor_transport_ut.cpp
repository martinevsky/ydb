// Transport tests of the Solomon sink (FQ transport test plan, target C): TLS and credentials, timeouts,
// blocking token fetch, retry classification, secrets in issues/logs, URL escaping.
// Contract tests that fail on today's code start with YDB_SKIP_KNOWN_BUG (see known_bug.h).

#include "ut_helpers.h"

#include <ydb/library/yql/providers/common/token_accessor/client/ut_helpers/fake_token_accessor.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/known_bug.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/loopback_http_server.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/test_pki.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/wait.h>
#include <ydb/library/yql/providers/solomon/actors/dq_solomon_write_actor_retry.h>

#include <library/cpp/cgiparam/cgiparam.h>
#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/scope.h>
#include <util/string/ascii.h>

#include <future>

namespace NYql::NDq {

using namespace NYql::NTransportTest;

namespace {

const TString Secret = "SECRET-TOKEN";
const TDuration Guard = TDuration::Seconds(20);

NSo::NProto::TDqSolomonShard ShardSettings(const TString& endpoint, bool useSsl) {
    auto settings = BuildSolomonShardSettings(/*isCloud=*/false); // CT_SOLOMON: "Authorization: OAuth <token>"
    settings.SetEndpoint(endpoint);
    settings.SetUseSsl(useSsl);
    settings.MutableToken()->SetName("tok");
    return settings;
}

TString PlainEndpoint(const TLoopbackHttpServer& server) {
    return TStringBuilder() << "127.0.0.1:" << server.Port();
}

TScriptedResponse PushOk() {
    return TScriptedResponse::Ok(SolomonPushOkBody()).AddHeader("Content-Type", "application/json");
}

bool LooksLikeCertificateError(const TString& text) {
    const TString lower = to_lower(text);
    return lower.Contains("certificate") || lower.Contains("verify");
}

// An NHttp response event as the sink's retry policy sees it, with the given status line.
struct TParsedResponse {
    NHttp::THttpOutgoingRequestPtr Request = NHttp::THttpOutgoingRequest::CreateRequestPost("http://localhost/api/v2/push");
    NHttp::THttpIncomingResponsePtr Response;

    explicit TParsedResponse(TStringBuf status) {
        Response = new NHttp::THttpIncomingResponse(Request);
        const TString raw = TStringBuilder() << "HTTP/1.1 " << status << " Status\r\nContent-Length: 0\r\n\r\n";
        Response->EnsureEnoughSpaceAvailable(raw.size());
        memcpy(Response->Pos(), raw.data(), raw.size());
        Response->Advance(raw.size());
        Y_ENSURE(Response->IsDone() && Response->Status == status, "cannot build a response with status " << status);
    }

    ERetryErrorClass RetryClass() const {
        NHttp::TEvHttpProxy::TEvHttpIncomingResponse event(Request, Response);
        return NSo::SinkHttpRetryClass(&event);
    }
};

} // namespace

Y_UNIT_TEST_SUITE(TDqSolomonWriteActorTransportTest) {

    // ---- TLS and credentials (R1) ----

    // Pin (H3 validation): a push over TLS to the NHttp server reaches it with the token.
    Y_UNIT_TEST(TlsPushReachesCaSignedServer) {
        TTestPki pki;
        TRecordingAsyncOutputCallbacks callbacks;
        TFakeCASetup setup;
        TNHttpTlsServer server(*setup.Runtime, pki.Leaf());

        InitAsyncOutput(setup, ShardSettings(server.Endpoint(), true), callbacks, IamTokenSecureParams(Secret), CreateStructuredTokenCredentialsFactory());
        WriteOneRow(setup);

        WaitUntil([&] { return server.RequestCount() > 0; }, Guard, "a push at the TLS server");
        const auto requests = server.Requests();
        UNIT_ASSERT_VALUES_EQUAL(requests[0].Authorization, "OAuth " + Secret);
        UNIT_ASSERT_STRING_CONTAINS(requests[0].Url, "/api/v2/push?project=cloudId1");
        UNIT_ASSERT_VALUES_EQUAL(callbacks.ErrorCount(), 0u);
    }

    // T-TLS-7 (contract, F-A-1 incl. F-B-1): the sink refuses a TLS server whose certificate it cannot verify
    // (self-signed) and never delivers the token. Today NHttp does not verify, so the token reaches the server.
    Y_UNIT_TEST(UntrustedTlsServerRefused) {
        YDB_SKIP_KNOWN_BUG("F-A-1");
        TTestPki pki;
        TRecordingAsyncOutputCallbacks callbacks;
        TFakeCASetup setup;
        TNHttpTlsServer server(*setup.Runtime, pki.SelfSignedLeaf());

        InitAsyncOutput(setup, ShardSettings(server.Endpoint(), true), callbacks, IamTokenSecureParams(Secret), CreateStructuredTokenCredentialsFactory());
        WriteOneRow(setup);

        // Sync on the first edge: an error at the sink, or a request at the server.
        WaitUntil([&] { return callbacks.ErrorCount() > 0 || server.RequestCount() > 0; }, Guard, "a sink error or a request at the server");
        UNIT_ASSERT_VALUES_EQUAL_C(server.RequestCount(), 0u,
            "the untrusted server received a push; Authorization='" << server.Requests()[0].Authorization << "'");
        const auto error = callbacks.WaitForError(0, Guard);
        UNIT_ASSERT_C(LooksLikeCertificateError(error.Text()), error.Text());
        UNIT_ASSERT_VALUES_EQUAL(server.RequestCount(), 0u);
    }

    // T-TLS-8 pin: today a plaintext push (UseSsl=false) carries the token.
    Y_UNIT_TEST(PlaintextPushCarriesTokenToday) {
        TLoopbackHttpServer server;
        server.SetDefault(PushOk());
        TRecordingAsyncOutputCallbacks callbacks;
        TFakeCASetup setup;

        InitAsyncOutput(setup, ShardSettings(PlainEndpoint(server), false), callbacks, IamTokenSecureParams(Secret), CreateStructuredTokenCredentialsFactory());
        WriteOneRow(setup);

        server.WaitForRequests(1, Guard);
        UNIT_ASSERT_VALUES_EQUAL(server.Requests()[0].Header("Authorization").GetOrElse(""), "OAuth " + Secret);
    }

    // T-TLS-8 contract (F-A-1, sibling of the read client): the sink never sends a token over plaintext.
    // Either the push goes without Authorization, or the sink fails fast without contacting the server.
    Y_UNIT_TEST(PlaintextPushCarriesNoToken) {
        YDB_SKIP_KNOWN_BUG("F-A-1");
        TLoopbackHttpServer server;
        server.SetDefault(PushOk());
        TRecordingAsyncOutputCallbacks callbacks;
        TFakeCASetup setup;

        InitAsyncOutput(setup, ShardSettings(PlainEndpoint(server), false), callbacks, IamTokenSecureParams(Secret), CreateStructuredTokenCredentialsFactory());
        WriteOneRow(setup);

        WaitUntil([&] { return server.RequestCount() > 0 || callbacks.ErrorCount() > 0; }, Guard, "a push at the server or a sink error");
        UNIT_ASSERT_C(!server.AnyRequestContains(Secret), "the token was sent over plaintext: " << server.Requests()[0].RawHead);
    }

    // ---- Timeouts (R2) ----

    // T-TMO-10: with a request timeout (seam S7) a push to an endpoint that accepts and never answers fails
    // with a timeout, and the client closes that connection.
    Y_UNIT_TEST(StalledPushTimesOut) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::StallForever());
        TRecordingAsyncOutputCallbacks callbacks;
        TFakeCASetup setup;

        InitAsyncOutput(setup, ShardSettings(PlainEndpoint(server), false), callbacks, IamTokenSecureParams(Secret),
            CreateStructuredTokenCredentialsFactory(), TDuration::Seconds(1));
        WriteOneRow(setup);

        const auto error = callbacks.WaitForError(0, Guard);
        UNIT_ASSERT_STRING_CONTAINS(error.Text(), "timed out");
        server.WaitConnectionClosedByPeer(0, Guard);
    }

    // ---- Lifetime (R4) ----

    // T-LIF-11 (contract, F-B-5): creating the sink does not block the actor thread on the token accessor.
    // The accessor holds every GetToken call until released; the creation (run on the fake compute actor)
    // must return while the call is still held. After the release the push carries the SA token.
    Y_UNIT_TEST(CreationDoesNotBlockOnTokenAccessor) {
        YDB_SKIP_KNOWN_BUG("F-B-5");
        TFakeTokenAccessor accessor("SA-TOKEN");
        accessor.GateReplies();
        TLoopbackHttpServer server;
        server.SetDefault(PushOk());
        // A 1 h request timeout, so the provider's bounded synchronous wait (timeout + 10 s) cannot end first.
        auto credentialsFactory = CreateStructuredTokenCredentialsOverTokenAccessorFactory(
            accessor.Endpoint(), false, "", 0, TDuration::Hours(2), TDuration::Hours(1));
        const THashMap<TString, TString> secureParams = {{"tok", TStructuredTokenBuilder().SetServiceAccountIdAuth("sa-id", "sa-signature").ToJson()}};
        TRecordingAsyncOutputCallbacks callbacks;
        TFakeCASetup setup;

        auto created = std::async(std::launch::async, [&] {
            InitAsyncOutput(setup, ShardSettings(PlainEndpoint(server), false), callbacks, secureParams, credentialsFactory);
        });
        // On every exit path open the gate first, so that a blocked creation finishes before `created` and
        // `setup` are destroyed.
        Y_DEFER {
            accessor.Release();
        };

        accessor.WaitForCalls(1, TDuration::Seconds(30));
        const bool returned = created.wait_for(std::chrono::seconds(30)) == std::future_status::ready;
        UNIT_ASSERT_C(returned && accessor.HeldNow() == 1,
            "sink creation blocked the actor thread while the token accessor call was held");
        created.get();

        accessor.Release();
        WriteOneRow(setup);
        WaitUntil([&] { return server.RequestCount() > 0 || callbacks.ErrorCount() > 0; }, TDuration::Seconds(30), "a push or a sink error");
        UNIT_ASSERT_VALUES_EQUAL_C(callbacks.ErrorCount(), 0u, callbacks.Errors()[0].Text());
        UNIT_ASSERT_VALUES_EQUAL(server.Requests()[0].Header("Authorization").GetOrElse(""), "OAuth SA-TOKEN");
    }

    // ---- Retry classification (R5) ----

    // T-RTY-10 (a), pin: the entries of the retry table on which today's code and the contract agree.
    Y_UNIT_TEST(RetryClassPinnedToday) {
        UNIT_ASSERT_EQUAL(NSo::SinkHttpRetryClass(nullptr), ERetryErrorClass::ShortRetry);
        {
            // No response at all: the connection was not established.
            auto request = NHttp::THttpOutgoingRequest::CreateRequestPost("http://localhost/api/v2/push");
            NHttp::TEvHttpProxy::TEvHttpIncomingResponse event(request, nullptr, "Connection refused");
            UNIT_ASSERT_EQUAL(NSo::SinkHttpRetryClass(&event), ERetryErrorClass::ShortRetry);
        }
        UNIT_ASSERT_EQUAL(TParsedResponse("401").RetryClass(), ERetryErrorClass::NoRetry);
        for (TStringBuf status : {"429", "500", "502", "503", "504"}) {
            UNIT_ASSERT_EQUAL_C(TParsedResponse(status).RetryClass(), ERetryErrorClass::ShortRetry, status);
        }
    }

    // T-RTY-10 (a), contract (solomon-fq #13): deterministic 4xx answers are not retried.
    // Today every status except 401 is retried (for up to 60 s).
    Y_UNIT_TEST(RetryClassDoesNotRetryDeterministic4xx) {
        YDB_SKIP_KNOWN_BUG("SFQ-13");
        for (TStringBuf status : {"400", "403", "404", "413"}) {
            UNIT_ASSERT_EQUAL_C(TParsedResponse(status).RetryClass(), ERetryErrorClass::NoRetry, status);
        }
    }

    // T-RTY-10 (b), contract (solomon-fq #13): a 403 push fails terminally after exactly one request.
    // The first reported error must already be terminal (EXTERNAL_ERROR): the sender then stops, so the
    // request count checked after it is final.
    Y_UNIT_TEST(Deterministic4xxIsNotRetried) {
        YDB_SKIP_KNOWN_BUG("SFQ-13");
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::WithStatus(403, "forbidden"));
        TRecordingAsyncOutputCallbacks callbacks;
        TFakeCASetup setup;

        InitAsyncOutput(setup, ShardSettings(PlainEndpoint(server), false), callbacks, IamTokenSecureParams(Secret), CreateStructuredTokenCredentialsFactory());
        WriteOneRow(setup);

        const auto error = callbacks.WaitForError(0, Guard);
        UNIT_ASSERT_STRING_CONTAINS(error.Text(), "403");
        UNIT_ASSERT_C(error.Status == NYql::NDqProto::StatusIds::EXTERNAL_ERROR,
            "the first 403 error is not terminal (status " << NYql::NDqProto::StatusIds::StatusCode_Name(error.Status) << "): it will be retried");
        UNIT_ASSERT_VALUES_EQUAL(server.RequestCount(), 1u);
    }

    // ---- Secrets in issues and logs (R6) ----

    // T-ERR-5 (contract, candidate N-5): a server that echoes the request's Authorization header in its error
    // body must not make the token appear in the sink's issues or logs. Today the sink appends the response
    // body verbatim (GetObfuscatedData masks only the response's own headers).
    Y_UNIT_TEST(NoSecretInIssuesOrLogs) {
        YDB_SKIP_KNOWN_BUG("N-5");
        TTestPki pki;
        auto log = std::make_shared<TCapturedLog>();
        TRecordingAsyncOutputCallbacks callbacks;
        TFakeCASetup setup(1, CreateCapturingLogBackend(log));
        TNHttpTlsServer server(*setup.Runtime, pki.Leaf());
        server.SetResponder([](const TNHttpServerRequest& request) {
            return std::make_pair(TString("400"), TString(TStringBuilder() << R"({"error":"bad request","authorization":")" << request.Authorization << R"("})"));
        });

        InitAsyncOutput(setup, ShardSettings(server.Endpoint(), true), callbacks, IamTokenSecureParams(Secret), CreateStructuredTokenCredentialsFactory());
        WriteOneRow(setup);

        const auto error = callbacks.WaitForError(0, Guard);
        // The sink logs every error it reports; wait for that line so the log check below is causal.
        WaitUntil([&] { return log->Contains("error response["); }, Guard, "the sink's error log line");
        UNIT_ASSERT_STRING_CONTAINS(server.Requests()[0].Authorization, Secret); // the stimulus really echoed it
        UNIT_ASSERT_C(!error.Text().Contains(Secret), "token in the sink issue: " << error.Text());
        UNIT_ASSERT_C(!log->Contains(Secret), "token in the actor logs");
    }

    // ---- Solomon URLs ----

    // T-SOL-2: the push URL escapes the project, cluster and service parameters.
    Y_UNIT_TEST(PushUrlEscapesParameters) {
        const TString url = GetSolomonUrl("h:1", true, "p&x=1", "c d", "s=1", NSo::NProto::ESolomonClusterType::CT_SOLOMON);
        UNIT_ASSERT_C(url.StartsWith("https://h:1/api/v2/push?"), url);
        const TCgiParameters params(TStringBuf(url).After('?'));
        UNIT_ASSERT_VALUES_EQUAL(params.Get("project"), "p&x=1");
        UNIT_ASSERT_VALUES_EQUAL(params.Get("cluster"), "c d");
        UNIT_ASSERT_VALUES_EQUAL(params.Get("service"), "s=1");
        UNIT_ASSERT_VALUES_EQUAL(params.size(), 3u);
    }
}

} // namespace NYql::NDq
