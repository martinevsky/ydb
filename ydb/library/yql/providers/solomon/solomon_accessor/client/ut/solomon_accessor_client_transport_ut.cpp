// Transport tests for the Solomon/Monium read accessor (target B of the FQ transport test plan,
// test_plan.md §4): the HTTP API calls over the curl gateway and GetData over gRPC.
//
// Every test runs in its own process (FORK_SUBTESTS + SPLIT_FACTOR in ya.make, §6.3 D-7): the curl
// gateway is a process singleton and its teardown (curl_global_cleanup -> c-ares) must not race with
// gRPC. Inside a test the gateway scope is declared first, so the gRPC fake, the HTTP fake and the
// client are destroyed before the last gateway reference is released.
//
// Waits are guarded sync points (futures, server arrivals, latches); a guard is a failure deadline,
// never the pass condition (§6.3 D-1). Contract tests that fail today start with
// YDB_SKIP_KNOWN_BUG_GTEST (run them with --test-env=YDB_TRANSPORT_RUN_KNOWN_BUGS=1).

#include <ydb/library/yql/providers/solomon/solomon_accessor/client/solomon_accessor_client.h>
#include <ydb/library/yql/providers/solomon/solomon_accessor/client/solomon_accessor_retry.h>
#include <ydb/library/yql/providers/solomon/solomon_accessor/client/ut_helpers/fake_data_service.h>

#include <ydb/library/yql/providers/common/http_gateway/mock/yql_http_scripted_gateway.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/credentials/fake_credentials.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/known_bug_gtest.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/loopback_http_server.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/refusing_port.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/test_pki.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/yql/gateway_scope.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/yql/log_capture.h>
#include <ydb/library/yql/providers/solomon/common/util.h>

#include <library/cpp/json/json_writer.h>
#include <library/cpp/retry/retry_policy.h>
#include <library/cpp/testing/gtest/gtest.h>

#include <util/generic/size_literals.h>
#include <util/stream/format.h>
#include <util/string/ascii.h>
#include <util/string/builder.h>
#include <util/string/split.h>

#include <future>
#include <thread>

namespace {

using namespace NYql;
using namespace NYql::NSo;
using namespace NYql::NTransportTest;

const TString TOKEN = "SECRET";
const TString PROJECT = "proj";
const TString LABELS_PATH = "/api/v2/projects/proj/sensors/names";
const TString DATA_PATH = "/api/v2/projects/proj/sensors/data";
const TString LABELS_OK = R"({"names":["host"]})";
const TInstant FROM = TInstant::Seconds(1000);
const TInstant TO = TInstant::Seconds(2000);

NSo::NProto::TDqSolomonSource MakeSource(
    const TString& httpEndpoint,
    const TString& grpcEndpoint,
    bool useSsl = false,
    NSo::NProto::ESolomonClusterType clusterType = NSo::NProto::CT_SOLOMON)
{
    NSo::NProto::TDqSolomonSource source;
    source.SetClusterType(clusterType);
    source.SetUseSsl(useSsl);
    source.SetProject(PROJECT);
    source.SetCluster("folder");
    source.SetHttpEndpoint(httpEndpoint);
    source.SetGrpcEndpoint(grpcEndpoint);
    source.MutableDownsampling()->SetDisabled(true);
    return source;
}

// Small retry delays (10-20 ms) keep retried tests short; the delays are never an oracle.
TSolomonReadActorConfig MakeConfig(ui64 maxRetries = 1) {
    google::protobuf::Map<TString, TString> settings;
    settings["retryMinDelayMs"] = "10";
    settings["retryMinLongRetryDelayMs"] = "15";
    settings["retryMaxDelayMs"] = "20";
    settings["retryMaxTimeSec"] = "30";
    auto cfg = ParseSolomonReadActorConfig(settings);
    cfg.RetryConfig.MaxRetries = maxRetries;
    return cfg;
}

THttpGatewayConfig WithCaFile(const TTestPki& pki) {
    THttpGatewayConfig config;
    config.SetCaFile(pki.CaFile());
    return config;
}

// The reference low-speed shape of test_plan.md T-TMO-2: no overall timeout, stall guard 2 s / 1 KB.
THttpGatewayConfig WithLowSpeedGuard(THttpGatewayConfig config = {}) {
    config.SetRequestTimeoutSeconds(0);
    config.SetLowSpeedTimeSeconds(2);
    config.SetLowSpeedBytesLimit(1024);
    return config;
}

std::shared_ptr<TFakeCredentialsProvider> MakeCredentials(TString token = TOKEN) {
    return std::make_shared<TFakeCredentialsProvider>(std::move(token));
}

template <typename T>
T WaitResult(const NThreading::TFuture<T>& future, TDuration guard, TStringBuf what) {
    Y_ENSURE(future.Wait(guard), "guard of " << guard << " expired waiting for " << what);
    return future.GetValue();
}

TString StatusName(EStatus status) {
    switch (status) {
        case STATUS_OK:
            return "STATUS_OK";
        case STATUS_RETRIABLE_ERROR:
            return "STATUS_RETRIABLE_ERROR";
        case STATUS_FATAL_ERROR:
            return "STATUS_FATAL_ERROR";
    }
    return "unknown";
}

TString JsonMessageBody(const TString& message) {
    NJson::TJsonValue json;
    json["message"] = message;
    return NJson::WriteJson(json, /*formatOutput*/ false);
}

bool ContainsIgnoreCase(TStringBuf haystack, TStringBuf needle) {
    return to_lower(TString(haystack)).Contains(to_lower(TString(needle)));
}

// Ends a test: the client and the gRPC fake go first, then the gateway (D-7).
void FinishTest(ISolomonAccessorClient::TPtr& client, TFakeDataService* dataService, TGatewayScope& gateway) {
    client.reset();
    if (dataService) {
        dataService->Shutdown();
    }
    gateway.Inspector().AssertSettled();
    gateway.Close();
}

} // namespace

// T-TLS-9 (R, F-A-1): with UseSsl=false the accessor never asks for the token and never sends it.
TEST(TSolomonAccessorTransportTest, PlaintextSourceWithholdsToken) {
    TGatewayScope gateway;
    TFakeDataService dataService;
    TLoopbackHttpServer server;
    server.SetDefault(TScriptedResponse::Ok(LABELS_OK));
    const auto credentials = MakeCredentials();

    auto client = ISolomonAccessorClient::Make(
        MakeSource(TStringBuilder() << "127.0.0.1:" << server.Port(), dataService.Endpoint()),
        credentials, MakeConfig(), gateway.Gateway());

    const auto labels = WaitResult(client->GetLabelNames({}, FROM, TO), TDuration::Seconds(10), "GetLabelNames");
    EXPECT_EQ(labels.Status, STATUS_OK) << labels.Error;
    const auto data = WaitResult(client->GetData(TString("{}"), FROM, TO), TDuration::Seconds(10), "GetData");
    EXPECT_EQ(data.Status, STATUS_OK) << data.Error;

    const auto requests = server.Requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_FALSE(requests[0].HasHeader("Authorization")) << requests[0].RawHead;
    EXPECT_FALSE(server.AnyRequestContains(TOKEN));
    const auto calls = dataService.Calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_FALSE(calls[0].HasMetadata("authorization")) << *calls[0].Metadata("authorization");
    EXPECT_EQ(credentials->Calls(), 0u);

    FinishTest(client, &dataService, gateway);
}

// T-TLS-10 (R, N, F-A-1): GetData over TLS verifies the server certificate. A self-signed server is
// rejected in the handshake, so the call never reaches the service and the token is never sent.
TEST(TSolomonAccessorTransportTest, GrpcTlsRejectsSelfSignedServer) {
    TTestPki pki;
    TGatewayScope gateway;
    TFakeDataService dataService({.Tls = pki.SelfSignedLeaf()});
    const auto credentials = MakeCredentials();

    auto client = ISolomonAccessorClient::Make(
        MakeSource("localhost:1", dataService.Endpoint(), /*useSsl*/ true),
        credentials, MakeConfig(), gateway.Gateway());

    // Guard 30 s: gRPC connect backoff.
    const auto data = WaitResult(client->GetData(TString("{}"), FROM, TO), TDuration::Seconds(30), "GetData");
    EXPECT_EQ(data.Status, STATUS_RETRIABLE_ERROR) << StatusName(data.Status) << ": " << data.Error;
    EXPECT_EQ(dataService.CallCount(), 0u);
    for (const auto& call : dataService.Calls()) {
        EXPECT_FALSE(call.HasMetadata("authorization"));
    }

    FinishTest(client, &dataService, gateway);
}

// T-TMO-2 (R, F-A-4 Solomon half): with the reference low-speed guard a stalled endpoint ends the call
// with an error, after exactly one retry (MaxRetries = 1).
TEST(TSolomonAccessorTransportTest, StalledHttpEndpointEndsWithError) {
    TGatewayScope gateway(WithLowSpeedGuard());
    TLoopbackHttpServer server;
    server.SetDefault(TScriptedResponse::StallForever());

    auto client = ISolomonAccessorClient::Make(
        MakeSource(TStringBuilder() << "127.0.0.1:" << server.Port(), "127.0.0.1:1"),
        MakeCredentials(), MakeConfig(/*maxRetries*/ 1), gateway.Gateway());

    const auto labels = WaitResult(client->GetLabelNames({}, FROM, TO), TDuration::Seconds(20), "GetLabelNames");
    EXPECT_NE(labels.Status, STATUS_OK);
    EXPECT_TRUE(labels.Error.Contains("too slow")) << labels.Error;
    EXPECT_TRUE(labels.Error.Contains("internal code: 28")) << labels.Error;
    EXPECT_EQ(server.RequestCount(), 2u);

    FinishTest(client, nullptr, gateway);
}

namespace {

// T-TMO-9 scenario: the service holds GetData; the future must become ready while the service still
// holds the call (the causal proof that the client, not the server, ended it).
void CheckGrpcDeadline(const TSolomonReadActorConfig& cfg, TDuration guard) {
    TGatewayScope gateway;
    TFakeDataService dataService;
    dataService.HoldUntilRelease();

    auto client = ISolomonAccessorClient::Make(
        MakeSource("127.0.0.1:1", dataService.Endpoint()), MakeCredentials(), cfg, gateway.Gateway());

    auto future = client->GetData(TString("{}"), FROM, TO);
    const bool readyWhileHeld = future.Wait(guard); // Release() comes only after this
    // The server side of the same call sees the cancellation too (checked while it is still held).
    const bool serverSawCancel = readyWhileHeld && [&]() {
        try {
            dataService.WaitObservedCancelled(TDuration::Seconds(10));
            return true;
        } catch (const std::exception& e) {
            ADD_FAILURE() << e.what();
            return false;
        }
    }();
    dataService.Release();
    ASSERT_TRUE(readyWhileHeld) << "GetData did not complete within " << guard.ToString() << " while the service held it";
    EXPECT_TRUE(serverSawCancel);
    const auto& data = future.GetValue();
    EXPECT_EQ(data.Status, STATUS_RETRIABLE_ERROR) << StatusName(data.Status) << ": " << data.Error;
    EXPECT_TRUE(ContainsIgnoreCase(data.Error, "deadline")) << data.Error;

    FinishTest(client, &dataService, gateway);
}

} // namespace

// T-TMO-9 (C, solomon-fq #9) with seam S7 set: GetData has a per-call deadline.
TEST(TSolomonAccessorTransportTest, GrpcGetDataHonoursPerCallDeadline) {
    auto cfg = MakeConfig();
    cfg.GrpcRequestTimeout = TDuration::Seconds(1);
    CheckGrpcDeadline(cfg, TDuration::Seconds(15));
}

// T-TMO-9 (C, solomon-fq #9) with the default config: today GetData has no deadline at all
// (GrpcRequestTimeout defaults to zero = none, and TGRpcClientConfig::Timeout is not applied), so a
// hung service hangs the read for ever.
TEST(TSolomonAccessorTransportTest, GrpcGetDataHasDeadlineByDefault) {
    YDB_SKIP_KNOWN_BUG_GTEST("solomon-fq#9");
    const auto cfg = MakeConfig();
    ASSERT_GT(cfg.GrpcRequestTimeout, TDuration::Zero()) << "no default gRPC deadline for GetData";
    ASSERT_LE(cfg.GrpcRequestTimeout, TDuration::Seconds(30)) << "the default deadline does not fit a SMALL test";
    CheckGrpcDeadline(cfg, cfg.GrpcRequestTimeout + TDuration::Seconds(10));
}

// T-BUD-7 (C, F-B-3): a metadata GET must not reserve 100 MB of the download budget or send Range.
// With a 150 MB budget two concurrent GetLabelNames must both reach the server.
TEST(TSolomonAccessorTransportTest, MetadataGetDoesNotReserve100Mb) {
    YDB_SKIP_KNOWN_BUG_GTEST("F-B-3");
    THttpGatewayConfig config;
    config.SetMaxSimulatenousDownloadsSize(150_MB);
    TGatewayScope gateway(config);
    TLoopbackHttpServer server;
    server.SetDefault(TScriptedResponse::Ok(LABELS_OK));
    server.Gate(LABELS_PATH);

    auto client = ISolomonAccessorClient::Make(
        MakeSource(TStringBuilder() << "127.0.0.1:" << server.Port(), "127.0.0.1:1"),
        MakeCredentials(), MakeConfig(), gateway.Gateway());

    auto first = client->GetLabelNames({}, FROM, TO);
    auto second = client->GetLabelNames({}, FROM, TO);
    bool bothArrived = true;
    try {
        server.WaitForRequests(2, TDuration::Seconds(10));
    } catch (const std::exception& e) {
        bothArrived = false;
        ADD_FAILURE() << e.what() << "; requests seen while gated: " << server.RequestCount()
                      << ", max concurrent: " << server.MaxConcurrent();
    }
    if (bothArrived) {
        EXPECT_EQ(server.MaxConcurrent(), 2u);
    }
    for (const auto& request : server.Requests()) {
        EXPECT_FALSE(request.HasHeader("Range")) << request.RawHead;
    }
    server.Release(LABELS_PATH);
    EXPECT_EQ(WaitResult(first, TDuration::Seconds(10), "first GetLabelNames").Status, STATUS_OK);
    EXPECT_EQ(WaitResult(second, TDuration::Seconds(10), "second GetLabelNames").Status, STATUS_OK);

    FinishTest(client, nullptr, gateway);
}

// T-BUD-8 (C, B, F-B-3 + F-B-4): a Solomon GET works under a 64 MB budget, and a response above the
// 100 MB cap fails with an error that names the cap (and no bad_alloc).
TEST(TSolomonAccessorTransportTest, SmallBudgetGetWorksAndSizeCapHolds) {
    YDB_SKIP_KNOWN_BUG_GTEST("F-B-3");
    THttpGatewayConfig config;
    config.SetMaxSimulatenousDownloadsSize(64_MB);
    TGatewayScope gateway(config);
    TLoopbackHttpServer server;
    // A server that ignores Range, so nothing but the client can enforce the cap.
    server.Script(LABELS_PATH, {
        TScriptedResponse::Ok(LABELS_OK),
        TScriptedResponse::BigBody(100_MB + 1).NoRange(),
    });

    auto client = ISolomonAccessorClient::Make(
        MakeSource(TStringBuilder() << "127.0.0.1:" << server.Port(), "127.0.0.1:1"),
        MakeCredentials(), MakeConfig(), gateway.Gateway());

    const auto small = WaitResult(client->GetLabelNames({}, FROM, TO), TDuration::Seconds(10), "GetLabelNames (case 1)");
    EXPECT_EQ(small.Status, STATUS_OK) << small.Error;
    EXPECT_EQ(small.Result.Labels, std::vector<TString>{"host"});
    EXPECT_EQ(server.RequestCount(), 1u);

    const auto big = WaitResult(client->GetLabelNames({}, FROM, TO), TDuration::Seconds(30), "GetLabelNames (case 2)");
    EXPECT_NE(big.Status, STATUS_OK);
    EXPECT_TRUE(ContainsIgnoreCase(big.Error, "size") || ContainsIgnoreCase(big.Error, "limit")
        || ContainsIgnoreCase(big.Error, "too large") || ContainsIgnoreCase(big.Error, "too big")) << big.Error;
    EXPECT_FALSE(big.Error.Contains("bad_alloc")) << big.Error;

    FinishTest(client, nullptr, gateway);
}

// T-BUD-10 (R, F-A-4 x F-B-3): the caller of a hung GET goes away; once the low-speed guard fires the
// 100 MB reservation is freed and the next caller's queued GET goes through.
TEST(TSolomonAccessorTransportTest, HungRequestFreesReservationAfterCallerGone) {
    THttpGatewayConfig config = WithLowSpeedGuard();
    config.SetMaxSimulatenousDownloadsSize(150_MB);
    TGatewayScope gateway(config);
    TLoopbackHttpServer server;
    // Only the very first request stalls; any later one (client 1's retry, client 2) is answered.
    server.SetHandler([](const TReceivedRequest& request) {
        return request.Index == 0 ? TScriptedResponse::StallForever() : TScriptedResponse::Ok(LABELS_OK);
    });
    const TString httpEndpoint = TStringBuilder() << "127.0.0.1:" << server.Port();

    {
        auto client = ISolomonAccessorClient::Make(
            MakeSource(httpEndpoint, "127.0.0.1:1"), MakeCredentials(), MakeConfig(/*maxRetries*/ 1), gateway.Gateway());
        auto future = client->GetLabelNames({}, FROM, TO);
        server.WaitForRequests(1);
        // The client and the future go out of scope here, with the request still stalled.
    }

    auto client = ISolomonAccessorClient::Make(
        MakeSource(httpEndpoint, "127.0.0.1:1"), MakeCredentials(), MakeConfig(/*maxRetries*/ 1), gateway.Gateway());
    const auto labels = WaitResult(client->GetLabelNames({}, FROM, TO), TDuration::Seconds(20), "second GetLabelNames");
    EXPECT_EQ(labels.Status, STATUS_OK) << labels.Error;
    EXPECT_GE(server.RequestCount(), 2u);
    gateway.Inspector().WaitValue("AllocatedMemory", 0);

    FinishTest(client, nullptr, gateway);
}

// T-LIF-8 (C, coverage gap): the client is destroyed with a GetData in flight. The destructor returns,
// and the future becomes ready with an error while the service still holds the call.
TEST(TSolomonAccessorTransportTest, ClientDestroyedWithGrpcCallInFlight) {
    TGatewayScope gateway;
    TFakeDataService dataService;
    dataService.HoldUntilRelease();

    auto client = ISolomonAccessorClient::Make(
        MakeSource("127.0.0.1:1", dataService.Endpoint()), MakeCredentials(), MakeConfig(), gateway.Gateway());
    auto future = client->GetData(TString("{}"), FROM, TO);
    dataService.WaitForHeldCalls(1);

    std::promise<void> destroyed;
    auto destroyedFuture = destroyed.get_future();
    std::thread destroyer([&client, &destroyed]() {
        client.reset();
        destroyed.set_value();
    });
    const bool destructorReturned = destroyedFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    const bool readyWhileHeld = destructorReturned && future.Wait(TDuration::Seconds(10));
    dataService.Release();
    destroyer.join();

    ASSERT_TRUE(destructorReturned) << "~TSolomonAccessorClient did not return within 10 s with a call in flight";
    ASSERT_TRUE(readyWhileHeld) << "the GetData future was not ready before the service released the call";
    const auto& data = future.GetValue();
    EXPECT_NE(data.Status, STATUS_OK);
    Cerr << "T-LIF-8: status=" << StatusName(data.Status) << " error='" << data.Error << "'" << Endl;

    ISolomonAccessorClient::TPtr none;
    FinishTest(none, &dataService, gateway);
}

// T-LIF-9 (R, coverage gap): the client is destroyed with an HTTP request in flight; the callback owns
// the promise, so the future still completes with the server's answer.
TEST(TSolomonAccessorTransportTest, ClientDestroyedWithHttpRequestInFlight) {
    TGatewayScope gateway;
    TLoopbackHttpServer server;
    server.SetDefault(TScriptedResponse::Ok(LABELS_OK));
    server.Gate(LABELS_PATH);

    auto client = ISolomonAccessorClient::Make(
        MakeSource(TStringBuilder() << "127.0.0.1:" << server.Port(), "127.0.0.1:1"),
        MakeCredentials(), MakeConfig(), gateway.Gateway());
    auto future = client->GetLabelNames({}, FROM, TO);
    server.WaitForRequests(1);
    client.reset();
    server.Release(LABELS_PATH);

    const auto labels = WaitResult(future, TDuration::Seconds(10), "GetLabelNames");
    EXPECT_EQ(labels.Status, STATUS_OK) << labels.Error;
    EXPECT_EQ(labels.Result.Labels, std::vector<TString>{"host"});

    FinishTest(client, nullptr, gateway);
}

// T-RTY-12 (R, F-A-12), pure part through seam S9: the HTTP retry classification.
TEST(TSolomonAccessorTransportTest, SolomonHttpRetryClassMatrix) {
    for (const CURLcode code : {CURLE_COULDNT_RESOLVE_PROXY, CURLE_COULDNT_RESOLVE_HOST, CURLE_COULDNT_CONNECT,
                                CURLE_OPERATION_TIMEDOUT, CURLE_SEND_ERROR, CURLE_RECV_ERROR, CURLE_GOT_NOTHING,
                                CURLE_PARTIAL_FILE, CURLE_HTTP2, CURLE_HTTP2_STREAM, CURLE_AGAIN}) {
        EXPECT_EQ(SolomonHttpRetryClass(code, 0), ERetryErrorClass::ShortRetry) << "curl code " << static_cast<int>(code);
    }
    for (const CURLcode code : {CURLE_URL_MALFORMAT, CURLE_UNSUPPORTED_PROTOCOL, CURLE_SSL_CONNECT_ERROR,
                                CURLE_PEER_FAILED_VERIFICATION, CURLE_WRITE_ERROR, CURLE_ABORTED_BY_CALLBACK}) {
        EXPECT_EQ(SolomonHttpRetryClass(code, 0), ERetryErrorClass::NoRetry) << "curl code " << static_cast<int>(code);
    }
    for (const long httpCode : {429L, 502L, 503L, 504L}) {
        EXPECT_EQ(SolomonHttpRetryClass(CURLE_OK, httpCode), ERetryErrorClass::ShortRetry) << "HTTP " << httpCode;
    }
    for (const long httpCode : {0L, 200L, 400L, 401L, 403L, 404L, 500L, 501L}) {
        EXPECT_EQ(SolomonHttpRetryClass(CURLE_OK, httpCode), ERetryErrorClass::NoRetry) << "HTTP " << httpCode;
    }
}

// T-RTY-12 (R, F-A-12): the retry classification end to end, (a) 503 then 200, (b) 404,
// (c) connection refused with MaxRetries = 2.
TEST(TSolomonAccessorTransportTest, HttpRetryClassificationEndToEnd) {
    TGatewayScope gateway;
    TLoopbackHttpServer server;
    TRefusingPort closed; // bound, not listening: connections are refused
    const TString httpEndpoint = TStringBuilder() << "127.0.0.1:" << server.Port();

    {
        // (a) 503 is retried; the second attempt succeeds.
        server.Script(LABELS_PATH, {TScriptedResponse::WithStatus(503), TScriptedResponse::Ok(LABELS_OK)});
        auto client = ISolomonAccessorClient::Make(
            MakeSource(httpEndpoint, "127.0.0.1:1"), MakeCredentials(), MakeConfig(2), gateway.Gateway());
        const auto labels = WaitResult(client->GetLabelNames({}, FROM, TO), TDuration::Seconds(10), "GetLabelNames (a)");
        EXPECT_EQ(labels.Status, STATUS_OK) << labels.Error;
        EXPECT_EQ(server.RequestCount(), 2u);
    }
    {
        // (b) 404 is not retried.
        const TString path = "/api/v2/projects/missing/sensors/names";
        server.Script(path, {TScriptedResponse::WithStatus(404)});
        auto source = MakeSource(httpEndpoint, "127.0.0.1:1");
        source.SetProject("missing");
        auto client = ISolomonAccessorClient::Make(source, MakeCredentials(), MakeConfig(2), gateway.Gateway());
        const auto labels = WaitResult(client->GetLabelNames({}, FROM, TO), TDuration::Seconds(10), "GetLabelNames (b)");
        EXPECT_NE(labels.Status, STATUS_OK);
        EXPECT_TRUE(labels.Error.Contains("HTTP 404")) << labels.Error;
        size_t missingRequests = 0;
        for (const auto& request : server.Requests()) {
            missingRequests += request.Path == path;
        }
        EXPECT_EQ(missingRequests, 1u);
    }
    {
        // (c) a refused connection is retried MaxRetries times: 3 connect attempts in total.
        auto client = ISolomonAccessorClient::Make(
            MakeSource(TStringBuilder() << "127.0.0.1:" << closed.Port(), "127.0.0.1:1"),
            MakeCredentials(), MakeConfig(2), gateway.Gateway());
        const auto labels = WaitResult(client->GetLabelNames({}, FROM, TO), TDuration::Seconds(10), "GetLabelNames (c)");
        EXPECT_NE(labels.Status, STATUS_OK);
        EXPECT_TRUE(labels.Error.Contains("internal code: 7")) << labels.Error;
        EXPECT_EQ(gateway.Inspector().Get("method=GET/curl_code=7/count"), 3);
    }

    gateway.Inspector().AssertSettled();
    gateway.Close();
}

namespace {

const std::vector<grpc::StatusCode> NON_OK_GRPC_CODES = {
    grpc::StatusCode::CANCELLED,
    grpc::StatusCode::UNKNOWN,
    grpc::StatusCode::INVALID_ARGUMENT,
    grpc::StatusCode::DEADLINE_EXCEEDED,
    grpc::StatusCode::NOT_FOUND,
    grpc::StatusCode::ALREADY_EXISTS,
    grpc::StatusCode::PERMISSION_DENIED,
    grpc::StatusCode::RESOURCE_EXHAUSTED,
    grpc::StatusCode::FAILED_PRECONDITION,
    grpc::StatusCode::ABORTED,
    grpc::StatusCode::OUT_OF_RANGE,
    grpc::StatusCode::UNIMPLEMENTED,
    grpc::StatusCode::INTERNAL,
    grpc::StatusCode::UNAVAILABLE,
    grpc::StatusCode::DATA_LOSS,
    grpc::StatusCode::UNAUTHENTICATED,
};

bool IsRetriableGrpcCode(grpc::StatusCode code) {
    return code == grpc::StatusCode::RESOURCE_EXHAUSTED
        || code == grpc::StatusCode::UNAVAILABLE
        || code == grpc::StatusCode::DEADLINE_EXCEEDED
        || code == grpc::StatusCode::INTERNAL
        || code == grpc::StatusCode::ABORTED;
}

} // namespace

// T-RTY-13 (R, F-A-12): gRPC status -> EStatus for all 16 non-OK codes.
TEST(TSolomonAccessorTransportTest, GrpcStatusMappingMatrix) {
    TGatewayScope gateway;
    TFakeDataService dataService;
    auto client = ISolomonAccessorClient::Make(
        MakeSource("127.0.0.1:1", dataService.Endpoint()), MakeCredentials(), MakeConfig(), gateway.Gateway());

    for (const auto code : NON_OK_GRPC_CODES) {
        dataService.ReturnStatus(code, TStringBuilder() << "fake status " << static_cast<int>(code));
        const auto data = WaitResult(client->GetData(TString("{}"), FROM, TO), TDuration::Seconds(10), "GetData");
        const EStatus expected = IsRetriableGrpcCode(code) ? STATUS_RETRIABLE_ERROR : STATUS_FATAL_ERROR;
        EXPECT_EQ(data.Status, expected) << "grpc code " << static_cast<int>(code) << ": " << data.Error;
    }
    EXPECT_EQ(dataService.CallCount(), NON_OK_GRPC_CODES.size());

    FinishTest(client, &dataService, gateway);
}

// T-RTY-14 (C, F-B-7): the GetData error text carries the gRPC code even when the message is empty.
TEST(TSolomonAccessorTransportTest, GrpcErrorTextCarriesCode) {
    YDB_SKIP_KNOWN_BUG_GTEST("F-B-7");
    TGatewayScope gateway;
    TFakeDataService dataService;
    auto client = ISolomonAccessorClient::Make(
        MakeSource("127.0.0.1:1", dataService.Endpoint()), MakeCredentials(), MakeConfig(), gateway.Gateway());

    for (const auto code : NON_OK_GRPC_CODES) {
        dataService.ReturnStatus(code, "");
        const auto data = WaitResult(client->GetData(TString("{}"), FROM, TO), TDuration::Seconds(10), "GetData");
        EXPECT_TRUE(ContainsIgnoreCase(data.Error, "grpc code")) << "'" << data.Error << "'";
        EXPECT_TRUE(data.Error.Contains(ToString(static_cast<int>(code)))) << "'" << data.Error << "'";
        EXPECT_FALSE(data.Error.EndsWith(": ")) << "'" << data.Error << "'";
    }

    FinishTest(client, &dataService, gateway);
}

namespace {

// About 70 MB on the wire: above the gRPC client's default 64 MB receive limit.
constexpr ui64 OVERSIZED_POINTS = 5'000'000;

} // namespace

// T-RTY-15 (R, F-B-6) pin: an oversized GetData response is RESOURCE_EXHAUSTED on the client, which is
// retriable, so the read actor's retry policy bounds it: MaxRetries = 3 gives 4 attempts. The policy's
// delays (10-20 ms) are not waited for: only its retry/no-retry decisions matter here.
TEST(TSolomonAccessorTransportTest, OversizedGetDataRetryIsBoundedByPolicy) {
    TGatewayScope gateway;
    TFakeDataService dataService;
    dataService.Oversized(OVERSIZED_POINTS);
    const auto cfg = MakeConfig(/*maxRetries*/ 3);
    auto client = ISolomonAccessorClient::Make(
        MakeSource("127.0.0.1:1", dataService.Endpoint()), MakeCredentials(), cfg, gateway.Gateway());

    // The read actor's GetData policy (dq_solomon_read_actor.cpp).
    auto policy = IRetryPolicy<TGetDataResponse>::GetExponentialBackoffPolicy(
        [](const TGetDataResponse& response) {
            return response.Status == STATUS_RETRIABLE_ERROR ? ERetryErrorClass::ShortRetry : ERetryErrorClass::NoRetry;
        },
        cfg.RetryConfig.MinDelay, cfg.RetryConfig.MinLongRetryDelay, cfg.RetryConfig.MaxDelay,
        cfg.RetryConfig.MaxRetries, cfg.RetryConfig.MaxTime);
    auto state = policy->CreateRetryState();

    ui32 attempts = 0;
    TGetDataResponse last;
    while (true) {
        last = WaitResult(client->GetData(TString("{}"), FROM, TO), TDuration::Seconds(30), "GetData");
        ++attempts;
        if (!state->GetNextRetryDelay(last)) {
            break;
        }
        ASSERT_LT(attempts, 10u) << "the retry policy does not stop";
    }
    EXPECT_EQ(attempts, 4u);
    EXPECT_EQ(dataService.CallCount(), 4u);
    EXPECT_EQ(last.Status, STATUS_RETRIABLE_ERROR);
    EXPECT_TRUE(last.Error.Contains("larger than max")) << last.Error;

    FinishTest(client, &dataService, gateway);
}

// T-RTY-15 decision-gated contract: an oversized response is deterministic, so it is FATAL and the read
// actor makes one attempt.
TEST(TSolomonAccessorTransportTest, OversizedGetDataIsFatal) {
    YDB_SKIP_KNOWN_BUG_GTEST("DG-T-RTY-15");
    TGatewayScope gateway;
    TFakeDataService dataService;
    dataService.Oversized(OVERSIZED_POINTS);
    auto client = ISolomonAccessorClient::Make(
        MakeSource("127.0.0.1:1", dataService.Endpoint()), MakeCredentials(), MakeConfig(3), gateway.Gateway());

    const auto data = WaitResult(client->GetData(TString("{}"), FROM, TO), TDuration::Seconds(30), "GetData");
    EXPECT_EQ(data.Status, STATUS_FATAL_ERROR) << StatusName(data.Status) << ": " << data.Error;
    EXPECT_TRUE(data.Error.Contains("larger than max")) << data.Error;

    FinishTest(client, &dataService, gateway);
}

// T-RTY-16 (C, F-B-8): a credentials-provider failure ("IAM-token not ready yet") is transient, so the
// GetData error is retriable; the next call reaches the service with the token.
TEST(TSolomonAccessorTransportTest, CredentialsProviderFailureIsRetriable) {
    YDB_SKIP_KNOWN_BUG_GTEST("F-B-8");
    TTestPki pki;
    TGatewayScope gateway;
    TFakeDataService dataService({.Tls = pki.Leaf()});
    const auto credentials = MakeCredentials("T");
    credentials->FailNext(1);
    auto cfg = MakeConfig();
    cfg.GrpcRootCertsPem = pki.Ca().CertPem;

    auto client = ISolomonAccessorClient::Make(
        MakeSource("localhost:1", dataService.Endpoint(), /*useSsl*/ true), credentials, cfg, gateway.Gateway());

    const auto first = WaitResult(client->GetData(TString("{}"), FROM, TO), TDuration::Seconds(30), "first GetData");
    EXPECT_EQ(first.Status, STATUS_RETRIABLE_ERROR) << StatusName(first.Status) << ": " << first.Error;
    const auto second = WaitResult(client->GetData(TString("{}"), FROM, TO), TDuration::Seconds(30), "second GetData");
    EXPECT_EQ(second.Status, STATUS_OK) << second.Error;
    const auto calls = dataService.Calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].Metadata("authorization"), TMaybe<TString>("OAuth T"));

    FinishTest(client, &dataService, gateway);
}

// T-ERR-1 (C, F-B-4): the HTTP processors surface the gateway's issues. The scripted gateway (H9,
// through seam S12) answers every call with CURLE_OK, no HTTP code and an issue, which is how the real
// gateway reports e.g. a download above the budget.
TEST(TSolomonAccessorTransportTest, HttpProcessorsSurfaceGatewayIssues) {
    YDB_SKIP_KNOWN_BUG_GTEST("F-B-4");
    auto gateway = TScriptedHttpGateway::Make();
    gateway->SetBufferedScript([](const TScriptedHttpGateway::TCall&) -> std::optional<IHTTPGateway::TResult> {
        return IHTTPGateway::TResult(CURLE_OK, TIssues{TIssue("gateway said X")});
    });
    auto client = ISolomonAccessorClient::Make(
        MakeSource("127.0.0.1:1", "127.0.0.1:1"), MakeCredentials(), MakeConfig(), gateway);

    const TInstant to = TInstant::Now();
    const TInstant from = to - TDuration::Days(8); // crosses the 7-day cutoff: GetPointsCount uses HTTP
    const auto check = [](const TString& method, const TString& error) {
        EXPECT_TRUE(error.Contains("gateway said X")) << method << ": " << error;
        EXPECT_FALSE(error.Contains("HTTP 0")) << method << ": " << error;
    };
    check("GetLabelNames", WaitResult(client->GetLabelNames({}, from, to), TDuration::Seconds(10), "GetLabelNames").Error);
    check("ListMetrics", WaitResult(client->ListMetrics({}, from, to), TDuration::Seconds(10), "ListMetrics").Error);
    check("ListMetricsLabels", WaitResult(client->ListMetricsLabels({}, from, to), TDuration::Seconds(10), "ListMetricsLabels").Error);
    check("GetPointsCount", WaitResult(client->GetPointsCount({}, from, to), TDuration::Seconds(10), "GetPointsCount").Error);
    EXPECT_EQ(gateway->CallCount(), 4u);
}

// T-ERR-4 (C, N, coverage gap): no secret in Solomon read issues or logs. Both servers echo what they
// received (HTTP: the request head in the JSON "message" of a 400; gRPC: the metadata in the INTERNAL
// status message), so a verbatim error surfaces the token.
TEST(TSolomonAccessorTransportTest, NoSecretInReadIssuesOrLogs) {
    YDB_SKIP_KNOWN_BUG_GTEST("T-ERR-4");
    TLogCapture log;
    TTestPki pki;
    TGatewayScope gateway(WithCaFile(pki));
    TFakeDataService dataService({.Tls = pki.Leaf()});
    dataService.EchoMetadataInMessage(grpc::StatusCode::INTERNAL);
    TLoopbackHttpServer server({.Tls = pki.Leaf()});
    server.SetHandler([](const TReceivedRequest& request) {
        return TScriptedResponse::WithStatus(400, JsonMessageBody(request.RawHead));
    });
    auto cfg = MakeConfig();
    cfg.GrpcRootCertsPem = pki.Ca().CertPem;

    auto client = ISolomonAccessorClient::Make(
        MakeSource(TStringBuilder() << "127.0.0.1:" << server.Port(), dataService.Endpoint(), /*useSsl*/ true),
        MakeCredentials(), cfg, gateway.Gateway());

    const auto labels = WaitResult(client->GetLabelNames({}, FROM, TO), TDuration::Seconds(10), "GetLabelNames");
    const auto data = WaitResult(client->GetData(TString("{}"), FROM, TO), TDuration::Seconds(30), "GetData");
    // The token did travel (otherwise the test proves nothing).
    ASSERT_TRUE(server.AnyRequestContains(TOKEN));
    ASSERT_EQ(dataService.CallCount(), 1u);
    ASSERT_TRUE(dataService.Calls()[0].Metadata("authorization").GetOrElse("").Contains(TOKEN));

    EXPECT_NE(labels.Status, STATUS_OK);
    EXPECT_FALSE(labels.Error.Contains(TOKEN)) << labels.Error;
    EXPECT_NE(data.Status, STATUS_OK);
    EXPECT_FALSE(data.Error.Contains(TOKEN)) << data.Error;
    EXPECT_NO_THROW(log.AssertNoSecret(TOKEN));

    FinishTest(client, &dataService, gateway);
}

namespace {

// 2 KB "message" with a control character near the start.
TString LongMessageWithControlChar() {
    TString message(2_KB, 'm');
    message[10] = '\x01';
    return message;
}

TString SanitizedPrefix(const TString& message) {
    TString prefix = message.substr(0, 1_KB);
    for (auto& c : prefix) {
        if (static_cast<unsigned char>(c) < ' ' && c != '\n' && c != '\t') {
            c = ' ';
        }
    }
    return prefix;
}

// Failure-message form of a possibly huge error: the first 100 bytes (control characters escaped)
// and the size. Printing a 70 KB string into the test log stalls the test runner.
TString Head(const TString& error) {
    TStringBuilder head;
    for (const char c : TStringBuf(error).Head(100)) {
        if (static_cast<unsigned char>(c) < ' ') {
            head << "\\x" << Hex(static_cast<unsigned char>(c), HF_FULL);
        } else {
            head << c;
        }
    }
    return head << (error.size() > 100 ? "..." : "") << " (" << error.size() << " bytes)";
}

// A JSON body above the 64 KB parse limit.
TString HugeMessageBody() {
    return JsonMessageBody(TString(70_KB, 'h'));
}

} // namespace

// T-ERR-6 (R, F-B-7), labels half (pin): the error description is bounded: the "message" is cut to
// 1 KB + "...", control characters become spaces, and a body above 64 KB gives just the status.
TEST(TSolomonAccessorTransportTest, LabelsErrorDescriptionIsBounded) {
    TGatewayScope gateway;
    TLoopbackHttpServer server;
    const TString message = LongMessageWithControlChar();
    server.Script(LABELS_PATH, {
        TScriptedResponse::WithStatus(500, JsonMessageBody(message)),
        TScriptedResponse::WithStatus(500, HugeMessageBody()),
    });
    auto client = ISolomonAccessorClient::Make(
        MakeSource(TStringBuilder() << "127.0.0.1:" << server.Port(), "127.0.0.1:1"),
        MakeCredentials(), MakeConfig(), gateway.Gateway());

    const auto bounded = WaitResult(client->GetLabelNames({}, FROM, TO), TDuration::Seconds(10), "GetLabelNames (2 KB)");
    const TString expectedBounded = TStringBuilder() << "Monitoring api get labels request failed with HTTP 500: "
        << SanitizedPrefix(message) << "...";
    EXPECT_TRUE(bounded.Error == expectedBounded) << Head(bounded.Error);
    const auto huge = WaitResult(client->GetLabelNames({}, FROM, TO), TDuration::Seconds(10), "GetLabelNames (70 KB)");
    EXPECT_TRUE(huge.Error == "Monitoring api get labels request failed with HTTP 500") << Head(huge.Error);
    EXPECT_EQ(server.RequestCount(), 2u);

    FinishTest(client, nullptr, gateway);
}

// T-ERR-6 (C, F-B-7), points-count half: the same bounds for GetPointsCount errors.
TEST(TSolomonAccessorTransportTest, PointsCountErrorDescriptionIsBounded) {
    YDB_SKIP_KNOWN_BUG_GTEST("F-B-7");
    TGatewayScope gateway;
    TLoopbackHttpServer server;
    const TString message = LongMessageWithControlChar();
    server.Script(DATA_PATH, {
        TScriptedResponse::WithStatus(500, JsonMessageBody(message)),
        TScriptedResponse::WithStatus(500, HugeMessageBody()),
    });
    auto client = ISolomonAccessorClient::Make(
        MakeSource(TStringBuilder() << "127.0.0.1:" << server.Port(), "127.0.0.1:1"),
        MakeCredentials(), MakeConfig(), gateway.Gateway());

    const TInstant to = TInstant::Now();
    const TInstant from = to - TDuration::Days(8); // crosses the 7-day cutoff: the HTTP path is used
    const auto bounded = WaitResult(client->GetPointsCount({}, from, to), TDuration::Seconds(10), "GetPointsCount (2 KB)");
    EXPECT_NE(bounded.Status, STATUS_OK);
    EXPECT_FALSE(bounded.Error.Contains('\x01')) << "control character in the error";
    EXPECT_TRUE(bounded.Error.Contains(SanitizedPrefix(message) + "...")) << Head(bounded.Error);
    EXPECT_LE(bounded.Error.size(), 1_KB + 200) << "unbounded description";
    const auto huge = WaitResult(client->GetPointsCount({}, from, to), TDuration::Seconds(10), "GetPointsCount (70 KB)");
    EXPECT_TRUE(huge.Error == "Monitoring api points count request failed with HTTP 500") << Head(huge.Error);
    EXPECT_EQ(server.RequestCount(), 2u);

    FinishTest(client, nullptr, gateway);
}

// T-SOL-1 (C, N, F-B-9): the project id cannot rewrite the Solomon URL. Either the client rejects it
// before sending anything, or it arrives as one escaped path segment.
TEST(TSolomonAccessorTransportTest, ProjectIdCannotRewriteUrl) {
    YDB_SKIP_KNOWN_BUG_GTEST("F-B-9");
    TGatewayScope gateway;
    TLoopbackHttpServer server;
    server.SetDefault(TScriptedResponse::Ok(LABELS_OK));
    auto source = MakeSource(TStringBuilder() << "127.0.0.1:" << server.Port(), "127.0.0.1:1");
    source.SetProject("a/b?x=1#f");
    auto client = ISolomonAccessorClient::Make(source, MakeCredentials(), MakeConfig(), gateway.Gateway());

    const auto labels = WaitResult(client->GetLabelNames({}, FROM, TO), TDuration::Seconds(10), "GetLabelNames");
    if (labels.Status != STATUS_OK && server.RequestCount() == 0) {
        EXPECT_TRUE(ContainsIgnoreCase(labels.Error, "project")) << labels.Error;
    } else {
        const auto requests = server.Requests();
        ASSERT_EQ(requests.size(), 1u);
        const TVector<TString> segments = StringSplitter(requests[0].Path).Split('/').SkipEmpty();
        EXPECT_EQ(segments.size(), 6u) << requests[0].Target;
        if (segments.size() > 3) {
            EXPECT_EQ(segments[3], "a%2Fb%3Fx%3D1%23f") << requests[0].Target;
        }
        const TVector<TString> params = StringSplitter(requests[0].Query).Split('&').SkipEmpty();
        for (const auto& param : params) {
            EXPECT_FALSE(param.StartsWith("x=")) << requests[0].Target;
        }
    }

    FinishTest(client, nullptr, gateway);
}

// T-SOL-3 (R, coverage gap): the auth scheme per cluster type over TLS, on both transports, plus the
// x-client-id header.
TEST(TSolomonAccessorTransportTest, AuthSchemePerClusterType) {
    TTestPki pki;
    TGatewayScope gateway(WithCaFile(pki));
    TFakeDataService dataService({.Tls = pki.Leaf()});
    TLoopbackHttpServer server({.Tls = pki.Leaf()});
    server.SetDefault(TScriptedResponse::Ok(LABELS_OK));
    auto cfg = MakeConfig();
    cfg.GrpcRootCertsPem = pki.Ca().CertPem;

    const std::vector<std::pair<NSo::NProto::ESolomonClusterType, TString>> cases = {
        {NSo::NProto::CT_SOLOMON, "OAuth T"},
        {NSo::NProto::CT_MONITORING, "Bearer T"},
        {NSo::NProto::CT_MONIUM, "Bearer T"},
    };
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto& [clusterType, expected] = cases[i];
        auto client = ISolomonAccessorClient::Make(
            MakeSource(TStringBuilder() << "127.0.0.1:" << server.Port(), dataService.Endpoint(), /*useSsl*/ true, clusterType),
            MakeCredentials("T"), cfg, gateway.Gateway());
        const auto labels = WaitResult(client->GetLabelNames({}, FROM, TO), TDuration::Seconds(10), "GetLabelNames");
        EXPECT_EQ(labels.Status, STATUS_OK) << labels.Error;
        const auto data = WaitResult(client->GetData(TString("{}"), FROM, TO), TDuration::Seconds(30), "GetData");
        EXPECT_EQ(data.Status, STATUS_OK) << data.Error;

        const auto requests = server.Requests();
        ASSERT_EQ(requests.size(), i + 1);
        EXPECT_EQ(requests[i].Header("Authorization"), TMaybe<TString>(expected)) << "cluster type " << static_cast<int>(clusterType);
        EXPECT_EQ(requests[i].Header("x-client-id"), TMaybe<TString>("yandex-query"));
        const auto calls = dataService.Calls();
        ASSERT_EQ(calls.size(), i + 1);
        EXPECT_EQ(calls[i].Metadata("authorization"), TMaybe<TString>(expected)) << "cluster type " << static_cast<int>(clusterType);
        EXPECT_EQ(calls[i].Metadata("x-client-id"), TMaybe<TString>("yandex-query"));
    }

    ISolomonAccessorClient::TPtr none;
    FinishTest(none, &dataService, gateway);
}
