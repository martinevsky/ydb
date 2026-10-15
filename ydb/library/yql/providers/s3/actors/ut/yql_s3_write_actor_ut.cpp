#include <ydb/library/yql/providers/common/http_gateway/yql_http_default_retry_policy.h>
#include <ydb/library/yql/providers/common/ut_helpers/dq_fake_ca.h>
#include <ydb/library/yql/providers/common/ut_helpers/async_credentials.h>
#include <ydb/library/yql/providers/s3/actors/yql_s3_write_actor.h>
#include <ydb/library/services/services.pb.h>
#include <yql/essentials/minikql/mkql_string_util.h>

namespace NYql::NDq {
namespace {
using namespace NActors;
using namespace NKikimr::NMiniKQL;

class TMultipartGateway final : public IHTTPGateway {
public:
    std::function<void()> OnInitiated;
    ui32 Creates = 0;
    ui32 Parts = 0;
    ui32 Deletes = 0;
    void Upload(TString url, THeaders, TString, TOnResult callback, bool, TRetryPolicy::TPtr, IHttpRequestContext::TPtr) override {
        if (url.EndsWith("?uploads")) {
            ++Creates;
            if (OnInitiated) OnInitiated();
            callback(TResult(TContent(R"(<InitiateMultipartUploadResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/"><UploadId>held-upload</UploadId></InitiateMultipartUploadResult>)", 200)));
        } else if (url.Contains("?partNumber=")) {
            ++Parts;
        } else {
            UNIT_FAIL("Unexpected multipart completion");
        }
    }
    void Delete(TString, THeaders, TOnResult callback, TRetryPolicy::TPtr, IHttpRequestContext::TPtr) override {
        ++Deletes;
        callback(TResult(TContent("", 204)));
    }
    void Download(TString, THeaders, size_t, size_t, TOnResult, TString, TRetryPolicy::TPtr, IHttpRequestContext::TPtr) override {
        UNIT_FAIL("Unexpected download");
    }
    TCancelHook Download(TString, THeaders, size_t, size_t, TOnDownloadStart, TOnNewDataPart, TOnDownloadFinish,
        const ::NMonitoring::TDynamicCounters::TCounterPtr&, IHttpRequestContext::TPtr) override {
        UNIT_FAIL("Unexpected streaming download");
        return {};
    }
    ui64 GetBuffersSizePerStream() override { return 0; }
    void UpdatePoolCaps(THashMap<TWorkScope, size_t>) override {}
};

class TS3Writer {
public:
    struct TCallbacks : IDqComputeActorAsyncOutput::ICallbacks {
        ui32 Errors = 0;
        TString Issues;
        bool Finished = false;
        void ResumeExecution(EResumeSource) override {}
        void OnAsyncOutputError(ui64, const TIssues& issues, NDqProto::StatusIds::StatusCode) override {
            ++Errors;
            Issues = issues.ToString();
        }
        void OnAsyncOutputStateSaved(TSinkState&&, ui64, const NDqProto::TCheckpoint&) override {}
        void OnAsyncOutputStateCommitted(ui64, const NDqProto::TCheckpoint&) override {}
        void OnAsyncOutputFinished(ui64) override { Finished = true; }
    } Callbacks;
    std::shared_ptr<NTests::TAsyncCredentialsFactory> Credentials = std::make_shared<NTests::TAsyncCredentialsFactory>();
    std::shared_ptr<TMultipartGateway> Gateway = std::make_shared<TMultipartGateway>();

    TS3Writer() {
        Runtime.AddLocalService(FakeActorId, TActorSetupCmd(new TFakeActor(
            std::make_shared<TAsyncInputPromises>(), std::make_shared<TAsyncOutputPromises>()), TMailboxType::Simple, 0));
        Runtime.Initialize();
        Runtime.GetLogSettings(0)->Append(NKikimrServices::EServiceKikimr_MIN,
            NKikimrServices::EServiceKikimr_MAX, NKikimrServices::EServiceKikimr_Name);
        Execute([&](TFakeActor& actor) {
            NS3::TSink settings;
            settings.SetUrl("http://unused.invalid/");
            settings.SetMultipart(true);
            settings.SetAtomicUploadCommit(true);
            auto [output, outputActor] = CreateS3WriteActor(actor.TypeEnv, *actor.FunctionRegistry, nullptr,
                Gateway, std::move(settings), 0, TCollectStatsLevel::None, "test", "output", {}, &Callbacks,
                Credentials, GetHTTPDefaultRetryPolicy());
            actor.InitAsyncOutput(output, outputActor);
        });
    }
    ~TS3Writer() { Stop(); }
    void Stop() { Execute([](TFakeActor& actor) { actor.Terminate(); }); }

    enum class ECompletion { Continue, Finish };
    void Write(ECompletion completion = ECompletion::Finish, size_t bytes = 65536) {
        Execute([=](TFakeActor& actor) {
            TUnboxedValueBatch batch;
            batch.emplace_back(MakeString(TString(bytes, 'x')));
            actor.DqAsyncOutput->SendData(std::move(batch), bytes, {}, completion == ECompletion::Finish);
        });
    }
    void Pump() { Runtime.SimulateSleep(TDuration::MilliSeconds(1)); }
    i64 FreeSpace() {
        i64 result = 0;
        Execute([&](TFakeActor& actor) { result = actor.DqAsyncOutput->GetFreeSpace(); });
        return result;
    }
private:
    void Execute(TCallback callback) {
        std::exception_ptr exception;
        auto promise = NThreading::NewPromise();
        Runtime.Send(new IEventHandle(FakeActorId, Runtime.AllocateEdgeActor(),
            new TEvPrivate::TEvExecute(promise, callback, exception)));
        TDispatchOptions options;
        options.CustomFinalCondition = [&] { return promise.HasValue(); };
        Runtime.DispatchEvents(options, TDuration::Seconds(10));
        UNIT_ASSERT(promise.HasValue());
        if (exception) std::rethrow_exception(exception);
    }
    TTestActorRuntimeBase Runtime{1};
    const TActorId FakeActorId{0, "FakeActor"};
};
} // namespace

Y_UNIT_TEST_SUITE(S3WriterAsynchronousCredentials) {
    Y_UNIT_TEST(LaterDirectWriteCompletesCredentialsOnFileWriter) {
        TS3Writer writer;
        writer.Write(TS3Writer::ECompletion::Continue, 8 * 1024 * 1024);
        writer.Pump();
        UNIT_ASSERT_VALUES_EQUAL(writer.Credentials->State->Calls, 1);
        writer.Credentials->Release("initial-token");
        writer.Pump();
        UNIT_ASSERT_VALUES_EQUAL(writer.Gateway->Creates, 1);
        UNIT_ASSERT_C(writer.Callbacks.Errors == 0, writer.Callbacks.Issues);
        UNIT_ASSERT_VALUES_EQUAL(writer.Gateway->Parts, 1);
        const auto calls = writer.Credentials->State->Calls;
        writer.Credentials->State->Held = true;
        // Multipart initialization is complete. This batch enters Go directly
        // from the compute actor, rather than from the file actor's event loop.
        writer.Write(TS3Writer::ECompletion::Continue, 8 * 1024 * 1024);
        writer.Pump();
        UNIT_ASSERT_VALUES_EQUAL(writer.Credentials->State->Calls, calls + 1);
        UNIT_ASSERT_VALUES_EQUAL(writer.Gateway->Parts, 1);
        writer.Credentials->Release("later-token");
        writer.Pump();
        UNIT_ASSERT_VALUES_EQUAL(writer.Gateway->Parts, 2);
        UNIT_ASSERT_VALUES_EQUAL(writer.Callbacks.Errors, 0);
        UNIT_ASSERT(!writer.Callbacks.Finished);
    }

    Y_UNIT_TEST(StoppedWriterDropsPendingPartAndRetainsIndependentCleanup) {
        TS3Writer writer;
        writer.Gateway->OnInitiated = [&] { writer.Credentials->State->Held = true; };
        writer.Write();
        writer.Pump();
        UNIT_ASSERT_VALUES_EQUAL(writer.Credentials->State->Calls, 1);
        writer.Credentials->Release("initial-token");
        writer.Pump();
        UNIT_ASSERT_VALUES_EQUAL(writer.Gateway->Creates, 1);
        UNIT_ASSERT_C(writer.Callbacks.Errors == 0, writer.Callbacks.Issues);
        UNIT_ASSERT_VALUES_EQUAL(writer.Credentials->State->Calls, 2);
        UNIT_ASSERT_VALUES_EQUAL(writer.Gateway->Parts, 0);
        writer.Stop();
        writer.Credentials->Release("late-token");
        writer.Pump();
        UNIT_ASSERT_VALUES_EQUAL(writer.Gateway->Parts, 0);
        UNIT_ASSERT_VALUES_EQUAL(writer.Gateway->Deletes, 1);
        UNIT_ASSERT_VALUES_EQUAL(writer.Callbacks.Errors, 0);
        UNIT_ASSERT(!writer.Callbacks.Finished);
    }
}
} // namespace NYql::NDq
