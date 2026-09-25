#include "yql_http_default_retry_policy.h"

#include <ydb/library/yql/providers/common/ut_helpers/transport/manual_clock.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/string/builder.h>

#include <algorithm>
#include <array>

namespace NYql {

using NTransportTest::TManualClock;

Y_UNIT_TEST_SUITE(THttpDefaultRetryPolicyTest) {

    Y_UNIT_TEST(RetriableCurlCode) {
        auto state = GetHTTPDefaultRetryPolicy()->CreateRetryState();
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_COULDNT_CONNECT, 0).Defined());
    }

    Y_UNIT_TEST(NonRetriableCurlCode) {
        auto state = GetHTTPDefaultRetryPolicy()->CreateRetryState();
        UNIT_ASSERT(!state->GetNextRetryDelay(CURLE_URL_MALFORMAT, 0).Defined());
    }

    Y_UNIT_TEST(RetriableHttpCodes) {
        for (long httpCode : {408, 425, 429, 500, 502, 503, 504}) {
            auto state = GetHTTPDefaultRetryPolicy()->CreateRetryState();
            UNIT_ASSERT_C(state->GetNextRetryDelay(CURLE_OK, httpCode).Defined(), httpCode);
        }
    }

    Y_UNIT_TEST(NonRetriableHttpCodes) {
        // 0 is the rare case when curl code is not available, e.g. manual cancelling
        for (long httpCode : {0, 200, 400, 403, 404, 501}) {
            auto state = GetHTTPDefaultRetryPolicy()->CreateRetryState();
            UNIT_ASSERT_C(!state->GetNextRetryDelay(CURLE_OK, httpCode).Defined(), httpCode);
        }
    }

    Y_UNIT_TEST(RetriesInternalServerError) {
        auto state = GetHTTPDefaultRetryPolicy()->CreateRetryState();
        TDuration prevDelay;
        for (size_t i = 0; i < 5; ++i) {
            auto delay = state->GetNextRetryDelay(CURLE_OK, 500);
            UNIT_ASSERT_C(delay.Defined(), i);
            // 500 is a long retry, so the delay starts from minLongRetryDelay (200ms) and grows.
            // RandomizeDelay returns half of the current delay plus a random part of the other half.
            UNIT_ASSERT_GE_C(*delay, TDuration::MilliSeconds(100), i);
            UNIT_ASSERT_LE_C(prevDelay, *delay, i);
            prevDelay = *delay;
        }
    }

    Y_UNIT_TEST(FqPolicyRetriesInternalServerError) {
        auto state = GetFqHTTPRetryPolicy()->CreateRetryState();
        for (size_t i = 0; i < 5; ++i) {
            auto delay = state->GetNextRetryDelay(CURLE_OK, 500);
            UNIT_ASSERT_C(delay.Defined(), i);
            UNIT_ASSERT_GE_C(*delay, TDuration::MilliSeconds(100), i);
        }
    }

    Y_UNIT_TEST(MaxRetries) {
        auto state = GetHTTPDefaultRetryPolicy(THttpRetryPolicyOptions{.MaxRetries = 2})->CreateRetryState();
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_COULDNT_CONNECT, 0).Defined());
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_COULDNT_CONNECT, 0).Defined());
        UNIT_ASSERT(!state->GetNextRetryDelay(CURLE_COULDNT_CONNECT, 0).Defined());
    }

    Y_UNIT_TEST(MaxTime) {
        // maxTime has to stay above minDelay (10ms), see Y_ASSERT in TExponentialBackoffPolicy
        auto state = GetHTTPDefaultRetryPolicy(THttpRetryPolicyOptions{.MaxTime = TDuration::MilliSeconds(50)})->CreateRetryState();
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_COULDNT_CONNECT, 0).Defined());
        Sleep(TDuration::MilliSeconds(150));
        UNIT_ASSERT(!state->GetNextRetryDelay(CURLE_COULDNT_CONNECT, 0).Defined());
    }

    Y_UNIT_TEST(ZeroMaxTimeMeansDefaultMaxTime) {
        // Zero has to be translated to the default maxTime, not passed through as an expired budget
        auto state = GetHTTPDefaultRetryPolicy(TDuration::Zero())->CreateRetryState();
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_COULDNT_CONNECT, 0).Defined());
    }

    Y_UNIT_TEST(DefaultOptionsUseYqlRetriedCurlCodes) {
        UNIT_ASSERT(THttpRetryPolicyOptions{}.RetriedCurlCodes == YqlRetriedCurlCodes());
    }

    Y_UNIT_TEST(CustomRetriedCurlCodes) {
        auto state = GetHTTPDefaultRetryPolicy(THttpRetryPolicyOptions{.RetriedCurlCodes = {CURLE_URL_MALFORMAT}})->CreateRetryState();
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_URL_MALFORMAT, 0).Defined());
        UNIT_ASSERT(!state->GetNextRetryDelay(CURLE_COULDNT_CONNECT, 0).Defined());
    }

    Y_UNIT_TEST(FqPolicyUsesFqRetriedCurlCodes) {
        // These codes are retried by the Fq policy only
        for (CURLcode curlCode : {CURLE_PARTIAL_FILE, CURLE_GOT_NOTHING, CURLE_COULDNT_RESOLVE_HOST}) {
            auto fqState = GetFqHTTPRetryPolicy()->CreateRetryState();
            UNIT_ASSERT_C(fqState->GetNextRetryDelay(curlCode, 0).Defined(), int(curlCode));

            auto defaultState = GetHTTPDefaultRetryPolicy()->CreateRetryState();
            UNIT_ASSERT_C(!defaultState->GetNextRetryDelay(curlCode, 0).Defined(), int(curlCode));
        }
    }

    Y_UNIT_TEST(FqPolicyRetriableHttpCodes) {
        auto state = GetFqHTTPRetryPolicy()->CreateRetryState();
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_OK, 503).Defined());
        UNIT_ASSERT(!state->GetNextRetryDelay(CURLE_OK, 404).Defined());
    }

    Y_UNIT_TEST(FqPolicySharesRetryBudgetBetweenDnsAndOtherErrors) {
        // Dns errors are not retried on their own schedule, they consume the shared budget
        auto state = GetFqHTTPRetryPolicy()->CreateRetryState();
        for (size_t i = 0; i < 10; ++i) {
            UNIT_ASSERT_C(state->GetNextRetryDelay(CURLE_COULDNT_RESOLVE_HOST, 0).Defined(), i);
        }
        // Backoff has grown past minDelay for the non dns error as well
        UNIT_ASSERT_GT(*state->GetNextRetryDelay(CURLE_COULDNT_CONNECT, 0), TDuration::MilliSeconds(10));
    }

    // T-RTY-7 (P1): the FQ dns retry budget on a manual clock (seam S2) instead of Sleep(11 s) x 3.
    Y_UNIT_TEST(FqPolicyDnsErrorsAreGivenUpEarly) {
        TManualClock clock;
        auto state = GetFqHTTPRetryPolicy(clock.AsFunction())->CreateRetryState();
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_COULDNT_RESOLVE_HOST, 0).Defined());

        // Dns errors get 10 seconds, everything else keeps the default 5 minutes
        clock.Advance(TDuration::Seconds(11));
        UNIT_ASSERT(!state->GetNextRetryDelay(CURLE_COULDNT_RESOLVE_HOST, 0).Defined());
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_COULDNT_CONNECT, 0).Defined());
    }

    Y_UNIT_TEST(FqPolicyDnsErrorBudgetRestartsAfterOtherError) {
        TManualClock clock;
        auto state = GetFqHTTPRetryPolicy(clock.AsFunction())->CreateRetryState();
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_COULDNT_RESOLVE_HOST, 0).Defined());
        clock.Advance(TDuration::Seconds(11));

        // The host has been resolved in between, so the dns budget starts over and the dns
        // error is retried even though the first dns error is more than 10 seconds old
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_COULDNT_CONNECT, 0).Defined());
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_COULDNT_RESOLVE_HOST, 0).Defined());
    }

    Y_UNIT_TEST(FqPolicyDnsErrorAfterOtherErrorsIsRetried) {
        TManualClock clock;
        auto state = GetFqHTTPRetryPolicy(clock.AsFunction())->CreateRetryState();
        // Other errors use up more than the dns budget before the first dns error arrives
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_COULDNT_CONNECT, 0).Defined());
        clock.Advance(TDuration::Seconds(11));
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_COULDNT_CONNECT, 0).Defined());

        // The dns budget is counted from the first dns error, not from the start of the session
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_COULDNT_RESOLVE_HOST, 0).Defined());
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_COULDNT_RESOLVE_HOST, 0).Defined());
    }

    // T-RTY-7 boundary: the dns budget is [first dns error, +10 s).
    Y_UNIT_TEST(FqPolicyDnsBudgetBoundary) {
        TManualClock clock;
        auto state = GetFqHTTPRetryPolicy(clock.AsFunction())->CreateRetryState();
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_COULDNT_RESOLVE_HOST, 0).Defined());
        clock.Advance(TDuration::MilliSeconds(9'999));
        UNIT_ASSERT(state->GetNextRetryDelay(CURLE_COULDNT_RESOLVE_HOST, 0).Defined());
        clock.Advance(TDuration::MilliSeconds(1));
        UNIT_ASSERT(!state->GetNextRetryDelay(CURLE_COULDNT_RESOLVE_HOST, 0).Defined());
    }

    // T-RTY-1 (P1): golden retry matrix of both gateway policies. Any classification change must show
    // up as a diff of these tables. First delays: >= 100 ms for LongRetry (http 408/425/429/5xx),
    // < 100 ms for ShortRetry (retried curl codes).
    Y_UNIT_TEST(GoldenRetryMatrix) {
        constexpr std::array defaultRetriedCurl = {
            CURLE_COULDNT_CONNECT, CURLE_WEIRD_SERVER_REPLY, CURLE_WRITE_ERROR, CURLE_READ_ERROR,
            CURLE_OPERATION_TIMEDOUT, CURLE_SSL_CONNECT_ERROR, CURLE_BAD_DOWNLOAD_RESUME, CURLE_SEND_ERROR,
            CURLE_RECV_ERROR, CURLE_NO_CONNECTION_AVAILABLE};
        constexpr std::array fqOnlyRetriedCurl = {CURLE_PARTIAL_FILE, CURLE_GOT_NOTHING, CURLE_COULDNT_RESOLVE_HOST};
        constexpr std::array retriedHttp = {408L, 425L, 429L, 500L, 502L, 503L, 504L};
        constexpr std::array httpCodes = {0L, 200L, 206L, 301L, 400L, 401L, 403L, 404L, 408L, 409L, 412L, 416L,
            425L, 429L, 500L, 501L, 502L, 503L, 504L};

        const auto contains = [](const auto& table, auto value) {
            return std::find(table.begin(), table.end(), value) != table.end();
        };
        struct TPolicy {
            TString Name;
            IHTTPGateway::TRetryPolicy::TPtr Policy;
            bool Fq = false;
        };
        const TPolicy policies[] = {{"default", GetHTTPDefaultRetryPolicy(), false}, {"fq", GetFqHTTPRetryPolicy(), true}};
        for (const auto& [name, policy, fq] : policies) {
            for (int code = CURLE_OK + 1; code < CURL_LAST; ++code) {
                const auto curlCode = static_cast<CURLcode>(code);
                const bool expected = contains(defaultRetriedCurl, curlCode) || (fq && contains(fqOnlyRetriedCurl, curlCode));
                const auto delay = policy->CreateRetryState()->GetNextRetryDelay(curlCode, 0);
                UNIT_ASSERT_VALUES_EQUAL_C(delay.Defined(), expected, name << " curl code " << code);
                if (delay) {
                    UNIT_ASSERT_LT_C(*delay, TDuration::MilliSeconds(100), name << " curl code " << code);
                }
            }
            for (const long httpCode : httpCodes) {
                const auto delay = policy->CreateRetryState()->GetNextRetryDelay(CURLE_OK, httpCode);
                UNIT_ASSERT_VALUES_EQUAL_C(delay.Defined(), contains(retriedHttp, httpCode), name << " http " << httpCode);
                if (delay) {
                    UNIT_ASSERT_GE_C(*delay, TDuration::MilliSeconds(100), name << " http " << httpCode);
                }
            }
        }
    }

    // T-RTY-5 (P2): backoff is bounded by maxDelay (30 s), and MaxRetries stops the sequence.
    Y_UNIT_TEST(BackoffIsBounded) {
        const std::pair<CURLcode, long> errors[] = {{CURLE_OK, 503L}, {CURLE_COULDNT_CONNECT, 0L}};
        for (const auto& [curlCode, httpCode] : errors) {
            const TString context = TStringBuilder() << "curl " << int(curlCode) << " http " << httpCode;
            auto state = GetHTTPDefaultRetryPolicy()->CreateRetryState();
            TDuration max;
            for (int i = 0; i < 60; ++i) {
                const auto delay = state->GetNextRetryDelay(curlCode, httpCode);
                UNIT_ASSERT_C(delay.Defined(), context << " draw " << i);
                UNIT_ASSERT_LE_C(*delay, TDuration::Seconds(30), context << " draw " << i);
                max = Max(max, *delay);
            }
            UNIT_ASSERT_GE_C(max, TDuration::Seconds(15), context << ": the backoff never reached maxDelay / 2");

            auto limited = GetHTTPDefaultRetryPolicy(THttpRetryPolicyOptions{.MaxRetries = 5})->CreateRetryState();
            for (int i = 0; i < 5; ++i) {
                UNIT_ASSERT_C(limited->GetNextRetryDelay(curlCode, httpCode).Defined(), context << " retry " << i);
            }
            UNIT_ASSERT_C(!limited->GetNextRetryDelay(curlCode, httpCode).Defined(), context);
        }
    }

}

} // namespace NYql
