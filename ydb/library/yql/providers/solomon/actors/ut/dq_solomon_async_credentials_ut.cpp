#include "ut_helpers.h"

#include <ydb/library/yql/providers/common/ut_helpers/async_credentials.h>
#include <ydb/library/actors/http/http_proxy.h>
#include <ydb/library/services/services.pb.h>
#include <yql/essentials/minikql/mkql_string_util.h>

namespace NYql::NDq {
namespace {
using namespace NActors;
using namespace NKikimr::NMiniKQL;

class TAsyncWriter {
public:
    struct TCallbacks : IDqComputeActorAsyncOutput::ICallbacks {
        ui32 Errors = 0;
        bool Finished = false;
        void ResumeExecution(EResumeSource) override {}
        void OnAsyncOutputError(ui64, const TIssues&, NDqProto::StatusIds::StatusCode) override { ++Errors; }
        void OnAsyncOutputStateSaved(TSinkState&&, ui64, const NDqProto::TCheckpoint&) override {}
        void OnAsyncOutputStateCommitted(ui64, const NDqProto::TCheckpoint&) override {}
        void OnAsyncOutputFinished(ui64) override { Finished = true; }
    } Callbacks;
    std::shared_ptr<NTests::TAsyncCredentialsFactory> Credentials = std::make_shared<NTests::TAsyncCredentialsFactory>();
    ui32 HttpRequests = 0;
    TVector<TString> Authorizations;

    TAsyncWriter() {
        Runtime.AddLocalService(FakeActorId, TActorSetupCmd(new TFakeActor(
            std::make_shared<TAsyncInputPromises>(), std::make_shared<TAsyncOutputPromises>()), TMailboxType::Simple, 0));
        Runtime.Initialize();
        Runtime.GetLogSettings(0)->Append(NKikimrServices::EServiceKikimr_MIN,
            NKikimrServices::EServiceKikimr_MAX, NKikimrServices::EServiceKikimr_Name);
        Runtime.SetObserverFunc([this](TAutoPtr<IEventHandle>& event) {
            if (event->GetTypeRewrite() == NHttp::TEvHttpProxy::TEvHttpOutgoingRequest::EventType) {
                ++HttpRequests;
                const NHttp::THeaders headers(event->Get<NHttp::TEvHttpProxy::TEvHttpOutgoingRequest>()->Request->Headers);
                Authorizations.emplace_back(headers.Get("Authorization"));
                return TTestActorRuntimeBase::EEventAction::DROP;
            }
            return TTestActorRuntimeBase::EEventAction::PROCESS;
        });
        Execute([&](TFakeActor& actor) {
            auto settings = BuildSolomonShardSettings(true);
            settings.SetEndpoint("unused.invalid:1");
            auto [output, outputActor] = CreateDqSolomonWriteActor(std::move(settings), 0,
                TCollectStatsLevel::None, "test", 1, {}, &Callbacks,
                MakeIntrusive<NMonitoring::TDynamicCounters>(), Credentials, 100000);
            actor.InitAsyncOutput(output, outputActor);
        });
    }
    ~TAsyncWriter() { Stop(); }
    void Stop() { Execute([](TFakeActor& actor) { actor.Terminate(); }); }

    enum class ECompletion { Continue, Finish };
    void Write(ECompletion completion = ECompletion::Finish) {
        Execute([completion](TFakeActor& actor) {
            TUnboxedValueBatch batch;
            batch.emplace_back(CreateStruct(actor.HolderFactory, {
                NUdf::TUnboxedValuePod(ui64(123000000)), MakeString("label"), NUdf::TUnboxedValuePod(ui32(7))}));
            actor.DqAsyncOutput->SendData(std::move(batch), 1, {}, completion == ECompletion::Finish);
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

Y_UNIT_TEST_SUITE(SolomonAsynchronousCredentials) {

    // Consumer contract: the same writer obtains authorization for each new
    // batch. Actual ALTER/ACL/provider resolution is covered in the KQP case;
    // this fixture holds the credential boundary to test transport ordering.
    Y_UNIT_TEST(LaterBatchesUseCurrentCredentialsOnTheSameWriter) {
        TAsyncWriter writer;
        for (const auto token : {"original-token", "altered-binding-token", "refreshed-token"}) {
            writer.Credentials->State->Held = true;
            const auto requestsBefore = writer.HttpRequests;
            const auto callsBefore = writer.Credentials->State->Calls;
            writer.Write(TAsyncWriter::ECompletion::Continue);
            writer.Pump();
            UNIT_ASSERT_VALUES_EQUAL(writer.Credentials->State->Calls, callsBefore + 1);
            UNIT_ASSERT_VALUES_EQUAL(writer.HttpRequests, requestsBefore);
            writer.Credentials->Release(token);
            writer.Pump();
            UNIT_ASSERT_VALUES_EQUAL(writer.HttpRequests, requestsBefore + 1);
            UNIT_ASSERT_VALUES_EQUAL(writer.Authorizations.back(), TString("Bearer ") + token);
            UNIT_ASSERT_VALUES_EQUAL(writer.Callbacks.Errors, 0);
            UNIT_ASSERT(!writer.Callbacks.Finished);
        }
        UNIT_ASSERT_VALUES_EQUAL(writer.Authorizations.size(), 3);
    }

    Y_UNIT_TEST(LaterAuthorizationDenialNeverReusesThePreviousBatchToken) {
        TAsyncWriter writer;
        writer.Write(TAsyncWriter::ECompletion::Continue);
        writer.Credentials->Release("first-batch-token");
        writer.Pump();
        UNIT_ASSERT_VALUES_EQUAL(writer.HttpRequests, 1);
        const auto before = writer.FreeSpace();
        writer.Credentials->State->Held = true;
        writer.Write(TAsyncWriter::ECompletion::Continue);
        writer.Pump();
        UNIT_ASSERT_VALUES_EQUAL(writer.Credentials->State->Calls, 2);
        UNIT_ASSERT(writer.FreeSpace() < before);
        writer.Credentials->Fail();
        writer.Pump();
        UNIT_ASSERT_VALUES_EQUAL(writer.HttpRequests, 1);
        UNIT_ASSERT_VALUES_EQUAL(writer.Callbacks.Errors, 1);
        UNIT_ASSERT_VALUES_EQUAL(writer.FreeSpace(), before);
        UNIT_ASSERT_VALUES_EQUAL(writer.Authorizations.size(), 1);
        UNIT_ASSERT(!writer.Callbacks.Finished);
    }

    Y_UNIT_TEST(StoppedWriterIgnoresLateCredentials) {
        TAsyncWriter writer;
        writer.Write();
        writer.Pump();
        UNIT_ASSERT_VALUES_EQUAL(writer.Credentials->State->Calls, 1);
        UNIT_ASSERT_VALUES_EQUAL(writer.HttpRequests, 0);
        writer.Stop();
        writer.Credentials->Release("late-token");
        writer.Pump();
        UNIT_ASSERT_VALUES_EQUAL(writer.HttpRequests, 0);
        UNIT_ASSERT_VALUES_EQUAL(writer.Callbacks.Errors, 0);
        UNIT_ASSERT(!writer.Callbacks.Finished);
    }

    Y_UNIT_TEST(HeldCredentialsDelayHttpAndCompletion) {
        TAsyncWriter writer;
        writer.Write();
        writer.Pump();
        UNIT_ASSERT_VALUES_EQUAL(writer.Credentials->State->Calls, 1);
        UNIT_ASSERT_VALUES_EQUAL(writer.HttpRequests, 0);
        UNIT_ASSERT(!writer.Callbacks.Finished);
        writer.Credentials->Release("minted-token");
        writer.Pump();
        UNIT_ASSERT_VALUES_EQUAL(writer.HttpRequests, 1);
        UNIT_ASSERT_VALUES_EQUAL(writer.Callbacks.Errors, 0);
        UNIT_ASSERT(!writer.Callbacks.Finished);
    }

    Y_UNIT_TEST(DeniedCredentialsRestoreBackpressureAndNeverSendHttp) {
        TAsyncWriter writer;
        const auto before = writer.FreeSpace();
        writer.Write();
        writer.Pump();
        UNIT_ASSERT(writer.FreeSpace() < before);
        writer.Credentials->Fail();
        writer.Pump();
        UNIT_ASSERT_VALUES_EQUAL(writer.HttpRequests, 0);
        UNIT_ASSERT_VALUES_EQUAL(writer.Callbacks.Errors, 1);
        UNIT_ASSERT_VALUES_EQUAL(writer.FreeSpace(), before);
        UNIT_ASSERT(!writer.Callbacks.Finished);
    }
}
} // namespace NYql::NDq
