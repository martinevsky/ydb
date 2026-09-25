#include "../yql_http_gateway.h"

#include <library/cpp/testing/common/env.h>
#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/testing/hook/hook.h>
#include <library/cpp/threading/future/core/future.h>

#include <util/generic/size_literals.h>
#include <util/stream/file.h>
#include <util/system/env.h>
#include <util/system/mutex.h>

#include <memory>

#include <ydb/library/aws_init/aws.h>

namespace {

// Moto S3 server started by ydb/tests/tools/s3_recipe (S3_ENDPOINT = http://localhost:<port>).
TString GetS3Endpoint() {
    const TString endpoint = GetEnv("S3_ENDPOINT");
    UNIT_ASSERT_C(endpoint, "S3_ENDPOINT is not set: the s3_recipe did not start");
    return endpoint;
}

// PUT without credentials, like the anonymous AWS client of ydb/library/testlib/s3_recipe_helper.
void Put(const NYql::IHTTPGateway::TPtr& gateway, const TString& url, const TString& body) {
    auto promise = NThreading::NewPromise<NYql::IHTTPGateway::TResult>();
    gateway->Upload(url, {}, body, [promise](NYql::IHTTPGateway::TResult&& result) mutable {
        promise.SetValue(std::move(result));
    }, true);
    auto future = promise.GetFuture();
    UNIT_ASSERT_C(future.Wait(TDuration::Seconds(30)), "PUT " << url << " did not complete");
    auto& result = future.GetValueSync();
    UNIT_ASSERT_C(!result.Issues, "PUT " << url << ": " << result.Issues.ToOneLineString());
    UNIT_ASSERT_VALUES_EQUAL_C(result.Content.HttpResponseCode, 200, "PUT " << url);
}

} // namespace

Y_TEST_HOOK_BEFORE_RUN(InitAwsAPI) {
    NKikimr::InitAwsAPI();
}

Y_TEST_HOOK_AFTER_RUN(ShutdownAwsAPI) {
    NKikimr::ShutdownAwsAPI();
}

Y_UNIT_TEST_SUITE(THttpGateway) {
    // T-E2E-2 (F-A-12): the real gateway retries a signed GET that failed with 404 and then succeeds.
    // The bucket does not exist when the first attempt is sent; the test creates it after the retry
    // policy has seen that 404, and a later attempt reads the object. Moto does not verify SigV4
    // signatures (target A T-SIG-2 does); the request still carries them.
    Y_UNIT_TEST(RetriesWithAwsCreds) {
        const auto httpGateway = NYql::IHTTPGateway::Make();

        // HTTP codes seen by the retry policy (it is asked after every attempt, including the last one).
        struct TSeenCodes {
            TMutex Mutex;
            TVector<long> Codes;
        };
        const auto seen = std::make_shared<TSeenCodes>();
        auto firstFailure = NThreading::NewPromise<void>();
        const auto retryPolicy = NYql::IHTTPGateway::TRetryPolicy::GetFixedIntervalPolicy(
            [seen, firstFailure](CURLcode, long httpCode) mutable {
                with_lock (seen->Mutex) {
                    seen->Codes.push_back(httpCode);
                }
                if (httpCode < 200 || httpCode >= 400) {
                    firstFailure.TrySetValue();
                    return ERetryErrorClass::LongRetry;
                }
                return ERetryErrorClass::NoRetry;
            },
            TDuration::MilliSeconds(500),
            TDuration::MilliSeconds(500),
            100,
            TDuration::Seconds(60)
        );

        const TString bucketUrl = GetS3Endpoint() + "/bucket";
        const TString requestUrl = bucketUrl + "/test.json";
        const auto awsUserPwd = "key:secret_key";
        const auto awsSigV4 = "aws:amz:ru-central-1:s3";
        const auto requestHeaders = NYql::IHTTPGateway::MakeYcHeaders("0", "", {}, awsUserPwd, awsSigV4);

        const TString expectedBody = TFileInput(ArcadiaFromCurrentLocation(__SOURCE_FILE__, "test.json")).ReadAll();
        UNIT_ASSERT(expectedBody);

        auto promise = NThreading::NewPromise<std::optional<TString>>();
        httpGateway->Download(
            requestUrl,
            requestHeaders,
            0,
            10_MB,
            [&promise, expectedBody](NYql::IHTTPGateway::TResult&& result) {
                const long code = result.Content.HttpResponseCode;
                const TString body = result.Content.Extract();

                std::optional<TString> error;
                if (result.Issues) {
                    error = result.Issues.ToOneLineString();
                } else if (code < 200 || code >= 300) {
                    error = TStringBuilder() << "Invalid http return code, expected 2xx, got " << code;
                } else if (body != expectedBody) {
                    error = TStringBuilder() << "Invalid value from S3, expected `" << expectedBody << "`, got `" << body << "`";
                }

                promise.SetValue(error);
            },
            {},
            retryPolicy
        );

        // The first attempt fails because the bucket does not exist yet; create it and the object now.
        UNIT_ASSERT_C(firstFailure.GetFuture().Wait(TDuration::Seconds(30)), "the first attempt did not fail");
        Put(httpGateway, bucketUrl, "");
        Put(httpGateway, requestUrl, expectedBody);

        auto future = promise.GetFuture();
        UNIT_ASSERT_C(future.Wait(TDuration::Seconds(90)), "the download did not complete");
        const auto error = future.ExtractValueSync();
        UNIT_ASSERT_C(error == std::nullopt, *error);

        with_lock (seen->Mutex) {
            UNIT_ASSERT_C(seen->Codes.size() >= 2, "no retry happened");
            UNIT_ASSERT_VALUES_EQUAL(seen->Codes.front(), 404);
            UNIT_ASSERT_C(seen->Codes.back() >= 200 && seen->Codes.back() < 300, "last code " << seen->Codes.back());
        }
    }
}
