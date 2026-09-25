// FQ transport test plan, target A, §4.9 (SigV4) and the gateway retry queue (§4.5): T-SIG-1..3,
// T-RTY-2, T-RTY-4.
//
// Time-dependent product logic (the SigV4 date, retry deadlines) runs on a manual clock through seam S1
// (NHttpGatewayTest::TGatewayTestOptions.Now). The clock never moves by itself: a retry scheduled on it
// becomes due only when the test advances it, so there are no backoff sleeps and no second-boundary
// races (rejected design RJ-2).

#include "yql_aws_signature.h"
#include "yql_http_default_retry_policy.h"
#include "yql_http_gateway_test_hooks.h"
#include "yql_http_gateway_ut_common.h"

#include <ydb/library/yql/providers/common/ut_helpers/transport/known_bug.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/loopback_http_server.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/manual_clock.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/datetime/systime.h>
#include <util/string/builder.h>
#include <util/string/hex.h>
#include <util/string/strip.h>

#include <openssl/sha.h>

#include <cstdio>

namespace NYql {

using namespace NTransportTest;
using namespace NHttpGatewayUt;

namespace {

const TString AWS_SIG_V4 = "aws:amz:ru-central1:s3";
const TString USER_PWD = "AKID:SECRET";

IHTTPGateway::THeaders AwsHeaders(const TString& contentType = {}) {
    return IHTTPGateway::MakeYcHeaders("req", {}, contentType, USER_PWD, AWS_SIG_V4);
}

TGatewayScope::TFactory WithClock(const TManualClock& clock) {
    return [now = clock.AsFunction()](const THttpGatewayConfig* config, ::NMonitoring::TDynamicCounterPtr counters) {
        return NHttpGatewayTest::MakeHttpGatewayForTest(config, std::move(counters), {.Now = now});
    };
}

TString AmzDate(TInstant instant) {
    return instant.FormatGmTime("%Y%m%dT%H%M%SZ");
}

TInstant ParseAmzDate(const TString& value) {
    struct tm tm = {};
    const int parsed = std::sscanf(value.c_str(), "%4d%2d%2dT%2d%2d%2dZ",
        &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    Y_ENSURE(parsed == 6, "bad x-amz-date: " << value);
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return TInstant::Seconds(TimeGM(&tm));
}

TString Sha256Hex(const TString& data) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
    return to_lower(HexEncode(digest, sizeof(digest)));
}

// T-SIG-2 verifier: recomputes the signature from what the server received (method, Host + target,
// body, the x-amz-date header and Content-Type if it is a signed header) and compares it with the
// received Authorization. Like S3, it honours SignedHeaders: curl adds an unsigned
// "Content-Type: application/x-www-form-urlencoded" to a POST without one, which S3 ignores.
void AssertSigV4Valid(const TReceivedRequest& request) {
    const TString date = request.Header("x-amz-date").GetOrElse("");
    const TString url = TStringBuilder() << "http://" << request.Header("Host").GetOrElse("") << request.Target;
    const bool contentTypeSigned = request.Header("Authorization").GetOrElse("").Contains("SignedHeaders=content-type;");
    const TString contentType = contentTypeSigned ? Strip(request.Header("Content-Type").GetOrElse("")) : TString();
    const TAwsSignature expected(request.Method, url, contentType, request.Body, AWS_SIG_V4, USER_PWD, ParseAmzDate(date));
    const TString context = TStringBuilder() << request.Method << " " << request.Target << " date " << date;
    UNIT_ASSERT_VALUES_EQUAL_C(request.Header("Authorization").GetOrElse(""), expected.GetAuthorization(), context);
    UNIT_ASSERT_VALUES_EQUAL_C(request.Header("x-amz-content-sha256").GetOrElse(""), Sha256Hex(request.Body), context);
    UNIT_ASSERT_VALUES_EQUAL_C(date, expected.GetAmzDate(), context);
}

// "Done() has finished for the `n`-th failed attempt": the result counter moved and the perform loop
// ran another cycle, so the retry is re-signed and sits in the Delayed queue.
void WaitRetryScheduled(const TGatewayScope& gateway, const TString& counter, i64 n) {
    gateway.Inspector().WaitValue(counter, n);
    WaitPerformCycles(gateway, 1);
}

} // namespace

Y_UNIT_TEST_SUITE(THttpGatewaySigV4Test) {

    // T-SIG-1 (F-A-8, P2): SigV4 is computed at dispatch, not at enqueue. A request that waits 20 min in
    // the queue must carry the dispatch-time date (S3 rejects a skew above 15 min). Today the request is
    // signed when it is created (TEasyCurl constructor -> InitHandles), so the date is the enqueue time.
    Y_UNIT_TEST(SignedAtDispatchNotAtEnqueue) {
        YDB_SKIP_KNOWN_BUG("F-A-8");
        TManualClock clock;
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("x").NoRange());
        server.Gate("/first");
        THttpGatewayConfig config;
        config.SetMaxInFlightCount(1);
        TGatewayScope gateway(config, WithClock(clock));

        TBufferedCall first, second;
        gateway->Download(server.Url("/first"), {}, 0, 0, first.Callback());
        server.WaitForRequests(1);
        const TInstant enqueued = clock.Now();
        gateway->Download(server.Url("/second"), AwsHeaders(), 0, 0, second.Callback());
        gateway.Inspector().WaitValue("AwaitQueue", 1);
        clock.Advance(TDuration::Minutes(20));
        server.Release("/first");
        UNIT_ASSERT_VALUES_EQUAL(first.Wait().HttpCode, 200);
        UNIT_ASSERT_VALUES_EQUAL(second.Wait().HttpCode, 200);

        const auto request = server.Requests().at(1);
        UNIT_ASSERT_VALUES_EQUAL_C(request.Header("x-amz-date").GetOrElse(""), AmzDate(enqueued + TDuration::Minutes(20)),
            "signed at enqueue time " << AmzDate(enqueued));
        AssertSigV4Valid(request);
        Finish(gateway);
    }

    // T-SIG-2 (F-A-12, P1): the signatures of real requests verify on the server side (replaces the
    // skipped ut_large signal). GET with Range, PUT with a body, POST ?uploads, DELETE ?uploadId=...,
    // and a key with a space, '+' and UTF-8 (percent-encoded, as the S3 provider sends it).
    Y_UNIT_TEST(ServerSideVerificationOfRealRequests) {
        TLoopbackHttpServer server;
        server.SetHandler([](const TReceivedRequest& request) {
            if (request.Method == "DELETE") {
                return TScriptedResponse::WithStatus(204);
            }
            return TScriptedResponse::Ok("0123456789");
        });
        TGatewayScope gateway;
        const TString object = server.Url("/bucket/data/obj.csv");

        TBufferedCall get, put, post, del, key;
        gateway->Download(object, AwsHeaders(), 2, 3, get.Callback());
        UNIT_ASSERT_VALUES_EQUAL(get.Wait().HttpCode, 206);
        gateway->Upload(object, AwsHeaders("application/octet-stream"), "payload \x01\xff", put.Callback(), true);
        UNIT_ASSERT_VALUES_EQUAL(put.Wait().HttpCode, 200);
        gateway->Upload(object + "?uploads", AwsHeaders(), "", post.Callback(), false);
        UNIT_ASSERT_VALUES_EQUAL(post.Wait().HttpCode, 200);
        gateway->Delete(object + "?uploadId=abc%2Fdef", AwsHeaders(), del.Callback());
        UNIT_ASSERT_VALUES_EQUAL(del.Wait().HttpCode, 204);
        gateway->Download(server.Url("/bucket/my%20key%2Bplus%D0%AF.csv"), AwsHeaders(), 0, 0, key.Callback());
        UNIT_ASSERT_VALUES_EQUAL(key.Wait().HttpCode, 200);

        const auto requests = server.Requests();
        UNIT_ASSERT_VALUES_EQUAL(requests.size(), 5);
        UNIT_ASSERT_VALUES_EQUAL(requests[0].Header("Range").GetOrElse(""), "bytes=2-4");
        UNIT_ASSERT_VALUES_EQUAL(requests[1].Body, "payload \x01\xff");
        UNIT_ASSERT_VALUES_EQUAL(requests[2].Method, "POST");
        UNIT_ASSERT_VALUES_EQUAL(requests[2].Query, "uploads");
        UNIT_ASSERT_VALUES_EQUAL(requests[3].Method, "DELETE");
        UNIT_ASSERT_VALUES_EQUAL(requests[4].Path, "/bucket/my%20key%2Bplus%D0%AF.csv");
        for (const auto& request : requests) {
            AssertSigV4Valid(request);
        }
        Finish(gateway);
    }

    // T-SIG-3 (F-A-8 alternative fix, P2, pin): with THeaders.Options.CurlSignature curl signs the
    // request itself (CURLOPT_AWS_SIGV4).
    Y_UNIT_TEST(CurlSignaturePathProducesValidHeader) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("x").NoRange());
        TGatewayScope gateway;
        auto headers = AwsHeaders();
        headers.Options.CurlSignature = true;
        const auto outcome = Download(gateway, server.Url("/bucket/obj"), std::move(headers));
        UNIT_ASSERT_VALUES_EQUAL_C(outcome.HttpCode, 200, outcome.Issues);
        const auto request = server.Requests().at(0);
        const TString authorization = request.Header("Authorization").GetOrElse("");
        UNIT_ASSERT_C(authorization.StartsWith("AWS4-HMAC-SHA256 Credential=AKID/"), authorization);
        UNIT_ASSERT_C(request.Header("x-amz-date").Defined(), request.RawHead);
        Finish(gateway);
    }

    // T-RTY-2 (F-A-8, P1, pin): the retry queue re-sends and re-signs. Today a retry is re-signed when
    // the failed attempt is handled (TEasyCurlBuffer::Reset in Done), not when it is dispatched: the
    // x-amz-date of attempt k+1 is the clock at the moment attempt k failed. (The dispatch-time contract
    // is T-SIG-1.) Each attempt is gated so the clock can be moved while it is held.
    Y_UNIT_TEST(RetryQueueResendsAndResigns) {
        TManualClock clock;
        TLoopbackHttpServer server;
        server.Script("/obj", {
            TScriptedResponse::WithStatus(503).Gated("attempt0"),
            TScriptedResponse::WithStatus(503).Gated("attempt1"),
            TScriptedResponse::Ok("done").NoRange()});
        server.Gate("attempt0");
        server.Gate("attempt1");
        TGatewayScope gateway({}, WithClock(clock));
        const TString failures = "method=GET/code=503/count";

        TBufferedCall call;
        TVector<TInstant> failedAt;
        const TInstant enqueued = clock.Now();
        gateway->Download(server.Url("/obj"), AwsHeaders(), 0, 0, call.Callback(), {}, GetHTTPDefaultRetryPolicy());
        for (i64 attempt = 0; attempt < 2; ++attempt) {
            server.WaitForRequests(attempt + 1);
            clock.Advance(TDuration::Seconds(1));
            failedAt.push_back(clock.Now());
            server.Release(TStringBuilder() << "attempt" << attempt);
            WaitRetryScheduled(gateway, failures, attempt + 1);
            // The backoff (<= 400 ms) is on the manual clock: nothing is re-sent until the clock moves.
            UNIT_ASSERT_VALUES_EQUAL(server.RequestCount(), attempt + 1);
            clock.Advance(TDuration::Seconds(1));
        }
        const auto outcome = call.Wait();
        UNIT_ASSERT_VALUES_EQUAL_C(outcome.HttpCode, 200, outcome.Issues);
        UNIT_ASSERT_VALUES_EQUAL(outcome.Body, "done");

        const auto requests = server.Requests();
        UNIT_ASSERT_VALUES_EQUAL(requests.size(), 3);
        const TVector<TInstant> expected = {enqueued, failedAt[0], failedAt[1]};
        for (size_t i = 0; i < requests.size(); ++i) {
            UNIT_ASSERT_VALUES_EQUAL_C(requests[i].Header("x-amz-date").GetOrElse(""), AmzDate(expected[i]), "attempt " << i);
            AssertSigV4Valid(requests[i]);
        }
        Finish(gateway);
        UNIT_ASSERT_VALUES_EQUAL(call.Calls(), 1);
    }

    // T-RTY-4 (P2): Retry-After is honoured and clamped to the policy's maxDelay (30 s). Known bug: the
    // retry policy has no access to response headers (IRetryPolicy<CURLcode, long>) and GET responses
    // do not even capture headers (CURLOPT_HEADERFUNCTION is PUT-only); seam S10 is not implemented.
    Y_UNIT_TEST(RetryAfterHonouredAndClamped) {
        YDB_SKIP_KNOWN_BUG("T-RTY-4");
        TManualClock clock;
        TLoopbackHttpServer server;
        server.Script("/a", {TScriptedResponse::WithStatus(429).AddHeader("Retry-After", "2"), TScriptedResponse::Ok("a").NoRange()});
        server.Script("/b", {TScriptedResponse::WithStatus(429).AddHeader("Retry-After", "3600"), TScriptedResponse::Ok("b").NoRange()});
        TGatewayScope gateway({}, WithClock(clock));
        const TString throttled = "method=GET/code=429/count";

        const auto check = [&](const TString& path, i64 failures, TDuration notYet, TDuration due) {
            const size_t before = server.RequestCount();
            TBufferedCall call;
            gateway->Download(server.Url(path), {}, 0, 0, call.Callback(), {}, GetHTTPDefaultRetryPolicy());
            WaitRetryScheduled(gateway, throttled, failures);
            const TInstant failedAt = clock.Now();
            clock.Set(failedAt + notYet);
            WaitPerformCycles(gateway);
            UNIT_ASSERT_VALUES_EQUAL_C(server.RequestCount(), before + 1,
                path << " was re-sent " << notYet << " after the 429, before the Retry-After deadline");
            clock.Set(failedAt + due);
            UNIT_ASSERT_VALUES_EQUAL(call.Wait().HttpCode, 200);
            UNIT_ASSERT_VALUES_EQUAL(server.RequestCount(), before + 2);
        };
        check("/a", 1, TDuration::Seconds(1), TDuration::Seconds(2));
        check("/b", 2, TDuration::Seconds(29), TDuration::Seconds(30));
        Finish(gateway);
    }
}

} // namespace NYql
