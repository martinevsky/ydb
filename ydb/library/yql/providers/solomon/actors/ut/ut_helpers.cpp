#include "ut_helpers.h"

#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/http/http_proxy.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/wait.h>
#include <yql/essentials/minikql/mkql_string_util.h>
#include <yql/essentials/providers/common/structured_token/yql_token_builder.h>

#include <library/cpp/http/simple/http_client.h>
#include <library/cpp/json/json_reader.h>
#include <library/cpp/retry/retry.h>
#include <library/cpp/testing/unittest/tests_data.h>

#include <util/system/guard.h>

#include <mutex>


namespace NYql::NDq {

using namespace NKikimr::NMiniKQL;

namespace {

void FillDqSolomonScheme(NSo::NProto::TDqSolomonShardScheme& scheme) {
    scheme.MutableTimestamp()->SetKey("ts");
    scheme.MutableTimestamp()->SetIndex(0);
    scheme.MutableTimestamp()->SetDataTypeId(NUdf::TDataType<NUdf::TTimestamp>::Id);

    NSo::NProto::TDqSolomonSchemeItem label;
    label.SetKey("label1");
    label.SetIndex(1);
    label.SetDataTypeId(NUdf::TDataType<ui32>::Id);

    NSo::NProto::TDqSolomonSchemeItem sensor;
    sensor.SetKey("sensor1");
    sensor.SetIndex(2);
    sensor.SetDataTypeId(NUdf::TDataType<ui32>::Id);

    scheme.MutableLabels()->Add(std::move(label));
    scheme.MutableSensors()->Add(std::move(sensor));
}

}

void InitAsyncOutput(
    TFakeCASetup& caSetup,
    NSo::NProto::TDqSolomonShard&& settings,
    i64 freeSpace)
{
    auto counters = MakeIntrusive<NMonitoring::TDynamicCounters>();
    const THashMap<TString, TString> secureParams;

    caSetup.Execute([&](TFakeActor& actor) {
        auto [dqAsyncOutput, dqAsyncOutputAsActor] = CreateDqSolomonWriteActor(
            std::move(settings),
            0,
            NYql::NDq::TCollectStatsLevel::None,
            "TxId-42",
            777,
            secureParams,
            &actor.GetAsyncOutputCallbacks(),
            counters,
            CreateStructuredTokenCredentialsFactory(),
            freeSpace);

        actor.InitAsyncOutput(dqAsyncOutput, dqAsyncOutputAsActor);
    });
}

NSo::NProto::TDqSolomonShard BuildSolomonShardSettings(bool isCloud) {
    NSo::NProto::TDqSolomonShard settings;
    settings.SetEndpoint(TString(getenv("SOLOMON_HTTP_ENDPOINT")));
    if (isCloud) {
        settings.SetProject("folderId1");
        settings.SetCluster("folderId1");
        settings.SetService("custom");
    } else {
        settings.SetProject("cloudId1");
        settings.SetCluster("folderId1");
        settings.SetService("custom");
    }

    settings.SetClusterType(isCloud ? NSo::NProto::ESolomonClusterType::CT_MONITORING : NSo::NProto::ESolomonClusterType::CT_SOLOMON);
    settings.SetUseSsl(false);

    FillDqSolomonScheme(*settings.MutableScheme());

    return settings;
}

NUdf::TUnboxedValue CreateStruct(
    NKikimr::NMiniKQL::THolderFactory& holderFactory,
    std::initializer_list<NUdf::TUnboxedValuePod> fields)
{
    NUdf::TUnboxedValue* itemsPtr = nullptr;
    auto structValues = holderFactory.CreateDirectArrayHolder(fields.size(), itemsPtr);
    for (auto&& field : fields) {
        *(itemsPtr++) = std::move(field);
    }
    return structValues;
}

int GetMetricsCount(TString metrics) {
    NJson::TJsonValue json;
    NJson::ReadJsonTree(metrics, &json, true);
    return json.GetArray().size();
}

// ---- TRecordingAsyncOutputCallbacks ----

void TRecordingAsyncOutputCallbacks::ResumeExecution(EResumeSource) {
}

void TRecordingAsyncOutputCallbacks::OnAsyncOutputError(ui64, const TIssues& issues, NYql::NDqProto::StatusIds::StatusCode fatalCode) {
    std::lock_guard lock(Mutex);
    Errors_.push_back(TError{.Issues = issues, .Status = fatalCode});
}

void TRecordingAsyncOutputCallbacks::OnAsyncOutputStateSaved(TSinkState&&, ui64, const NDqProto::TCheckpoint&) {
}

void TRecordingAsyncOutputCallbacks::OnAsyncOutputStateCommitted(ui64, const NDqProto::TCheckpoint&) {
}

void TRecordingAsyncOutputCallbacks::OnAsyncOutputFinished(ui64) {
}

TVector<TRecordingAsyncOutputCallbacks::TError> TRecordingAsyncOutputCallbacks::Errors() const {
    std::lock_guard lock(Mutex);
    return Errors_;
}

size_t TRecordingAsyncOutputCallbacks::ErrorCount() const {
    std::lock_guard lock(Mutex);
    return Errors_.size();
}

TRecordingAsyncOutputCallbacks::TError TRecordingAsyncOutputCallbacks::WaitForError(size_t index, TDuration guard) const {
    NYql::NTransportTest::WaitUntil([&] { return ErrorCount() > index; }, guard, TStringBuilder() << "sink error #" << index);
    std::lock_guard lock(Mutex);
    return Errors_[index];
}

void InitAsyncOutput(
    TFakeCASetup& caSetup,
    NSo::NProto::TDqSolomonShard&& settings,
    TRecordingAsyncOutputCallbacks& callbacks,
    const THashMap<TString, TString>& secureParams,
    IStructuredTokenCredentialsFactory::TPtr credentialsFactory,
    TDuration requestTimeout)
{
    auto counters = MakeIntrusive<NMonitoring::TDynamicCounters>();
    caSetup.Execute([&](TFakeActor& actor) {
        auto [dqAsyncOutput, dqAsyncOutputAsActor] = CreateDqSolomonWriteActor(
            std::move(settings),
            0,
            NYql::NDq::TCollectStatsLevel::None,
            "TxId-42",
            777,
            secureParams,
            &callbacks,
            counters,
            credentialsFactory,
            100000,
            false,
            requestTimeout);

        actor.InitAsyncOutput(dqAsyncOutput, dqAsyncOutputAsActor);
    });
}

void WriteOneRow(TFakeCASetup& caSetup) {
    caSetup.AsyncOutputWrite([](NKikimr::NMiniKQL::THolderFactory& holderFactory) {
        NKikimr::NMiniKQL::TUnboxedValueBatch res;
        res.emplace_back(CreateStruct(holderFactory, {
            NUdf::TUnboxedValuePod(static_cast<NUdf::TDataType<NUdf::TTimestamp>::TLayout>(1624811684)),
            NKikimr::NMiniKQL::MakeString("123"),
            NUdf::TUnboxedValuePod(678)
        }));
        return res;
    });
}

THashMap<TString, TString> IamTokenSecureParams(const TString& token) {
    return {{"tok", TStructuredTokenBuilder().SetIAMToken(token).ToJson()}};
}

TString SolomonPushOkBody() {
    return R"({"sensorsProcessed":1,"writtenMetricsCount":1})";
}

// ---- H3: TNHttpTlsServer ----

struct TNHttpTlsServer::TState {
    std::mutex Mutex;
    TVector<TNHttpServerRequest> Requests;
    TResponder Responder;
    TPortManager PortManager;
};

namespace {

class TNHttpRecordingHandler : public NActors::TActor<TNHttpRecordingHandler> {
public:
    explicit TNHttpRecordingHandler(std::shared_ptr<TNHttpTlsServer::TState> state)
        : TActor(&TNHttpRecordingHandler::StateFunc)
        , State(std::move(state))
    {}

    STRICT_STFUNC(StateFunc,
        hFunc(NHttp::TEvHttpProxy::TEvHttpIncomingRequest, Handle);
    )

private:
    void Handle(NHttp::TEvHttpProxy::TEvHttpIncomingRequest::TPtr& ev) {
        const auto& request = ev->Get()->Request;
        TNHttpServerRequest recorded;
        recorded.Url = TString(request->URL);
        recorded.Authorization = TString(NHttp::THeaders(request->Headers).Get("Authorization"));
        recorded.Headers = TString(request->Headers);
        recorded.Body = TString(request->Body);

        TNHttpTlsServer::TResponder responder;
        {
            // Recorded before the answer is sent, so the client's response happens after it.
            std::lock_guard lock(State->Mutex);
            State->Requests.push_back(recorded);
            responder = State->Responder;
        }
        auto [status, body] = responder ? responder(recorded) : std::make_pair(TString("200"), SolomonPushOkBody());
        const TString message = status == "200" ? "OK" : "Error";
        Send(ev->Sender, new NHttp::TEvHttpProxy::TEvHttpOutgoingResponse(request->CreateResponse(status, message, "application/json", body)));
    }

    const std::shared_ptr<TNHttpTlsServer::TState> State;
};

} // namespace

TNHttpTlsServer::TNHttpTlsServer(NActors::TTestActorRuntimeBase& runtime, const NYql::NTransportTest::TTestCert& cert, const TString& path)
    : State(std::make_shared<TState>())
{
    Port_ = State->PortManager.GetTcpPort();
    const NActors::TActorId proxy = runtime.Register(NHttp::CreateHttpProxy());
    auto add = MakeHolder<NHttp::TEvHttpProxy::TEvAddListeningPort>(Port_);
    add->Secure = true;
    add->CertificateFile = cert.CertFile;
    add->PrivateKeyFile = cert.KeyFile;
    const NActors::TActorId edge = runtime.AllocateEdgeActor();
    runtime.Send(new NActors::IEventHandle(proxy, edge, add.Release()), 0, true);
    Y_ENSURE(runtime.GrabEdgeEvent<NHttp::TEvHttpProxy::TEvConfirmListen>(edge), "the NHttp TLS listener did not start");

    const NActors::TActorId handler = runtime.Register(new TNHttpRecordingHandler(State));
    runtime.Send(new NActors::IEventHandle(proxy, handler, new NHttp::TEvHttpProxy::TEvRegisterHandler(path, handler)), 0, true);
}

void TNHttpTlsServer::SetResponder(TResponder responder) {
    std::lock_guard lock(State->Mutex);
    State->Responder = std::move(responder);
}

TString TNHttpTlsServer::Endpoint() const {
    return TStringBuilder() << "localhost:" << Port_;
}

size_t TNHttpTlsServer::RequestCount() const {
    std::lock_guard lock(State->Mutex);
    return State->Requests.size();
}

TVector<TNHttpServerRequest> TNHttpTlsServer::Requests() const {
    std::lock_guard lock(State->Mutex);
    return State->Requests;
}

// ---- TCapturedLog ----

void TCapturedLog::Append(TStringBuf data) {
    std::lock_guard lock(Mutex);
    Text_.append(data);
}

TString TCapturedLog::Text() const {
    std::lock_guard lock(Mutex);
    return Text_;
}

bool TCapturedLog::Contains(TStringBuf needle) const {
    std::lock_guard lock(Mutex);
    return Text_.Contains(needle);
}

namespace {

class TCapturingLogBackend : public TLogBackend {
public:
    explicit TCapturingLogBackend(std::shared_ptr<TCapturedLog> log)
        : Log(std::move(log))
    {}

    void WriteData(const TLogRecord& rec) override {
        const TStringBuf data(rec.Data, rec.Len);
        Log->Append(data);
        Cerr << data;
    }

    void ReopenLog() override {
    }

private:
    const std::shared_ptr<TCapturedLog> Log;
};

} // namespace

TAutoPtr<TLogBackend> CreateCapturingLogBackend(std::shared_ptr<TCapturedLog> log) {
    return new TCapturingLogBackend(std::move(log));
}

} // namespace NYql::NDq
