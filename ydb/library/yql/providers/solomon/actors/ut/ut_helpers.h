#pragma once

#include <ydb/library/yql/providers/common/ut_helpers/dq_fake_ca.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/test_pki.h>
#include <ydb/library/yql/providers/solomon/actors/dq_solomon_write_actor.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_holders.h>
#include <ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h>
#include <ydb/library/yql/dq/actors/protos/dq_events.pb.h>
#include <yql/essentials/minikql/mkql_alloc.h>

#include <ydb/library/actors/testlib/test_runtime.h>

#include <library/cpp/logger/backend.h>
#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/vector.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

namespace NYql::NDq {

void InitAsyncOutput(
    TFakeCASetup& caSetup,
    NSo::NProto::TDqSolomonShard&& settings,
    i64 freeSpace = 100000);

NSo::NProto::TDqSolomonShard BuildSolomonShardSettings(bool isCloud);

NUdf::TUnboxedValue CreateStruct(
    NKikimr::NMiniKQL::THolderFactory& holderFactory,
    std::initializer_list<NUdf::TUnboxedValuePod> fields);

int GetMetricsCount(TString metrics);

// ---- Transport test helpers (FQ transport test plan, target C) ----
// Every wait takes a guard that is a failure deadline (the helper throws), never a pass condition.

// Records the async output callbacks of the sink under test, including the status code that
// TFakeActor's callbacks drop. The sink calls them on its actor thread; the test reads them from its own.
class TRecordingAsyncOutputCallbacks : public IDqComputeActorAsyncOutput::ICallbacks {
public:
    struct TError {
        TIssues Issues;
        NYql::NDqProto::StatusIds::StatusCode Status = NYql::NDqProto::StatusIds::UNSPECIFIED;

        TString Text() const {
            return Issues.ToOneLineString();
        }
    };

    void ResumeExecution(EResumeSource source) override;
    void OnAsyncOutputError(ui64 outputIndex, const TIssues& issues, NYql::NDqProto::StatusIds::StatusCode fatalCode) override;
    void OnAsyncOutputStateSaved(TSinkState&& state, ui64 outputIndex, const NDqProto::TCheckpoint& checkpoint) override;
    void OnAsyncOutputStateCommitted(ui64 outputIndex, const NDqProto::TCheckpoint& checkpoint) override;
    void OnAsyncOutputFinished(ui64 outputIndex) override;

    TVector<TError> Errors() const;
    size_t ErrorCount() const;
    // Returns the index-th reported error; throws when the guard expires first.
    TError WaitForError(size_t index = 0, TDuration guard = TDuration::Seconds(20)) const;

private:
    mutable std::mutex Mutex;
    TVector<TError> Errors_;
};

// Creates the Solomon write actor with explicit callbacks, secure params, credentials and request
// timeout (seam S7). `callbacks` must outlive `caSetup`.
void InitAsyncOutput(
    TFakeCASetup& caSetup,
    NSo::NProto::TDqSolomonShard&& settings,
    TRecordingAsyncOutputCallbacks& callbacks,
    const THashMap<TString, TString>& secureParams,
    IStructuredTokenCredentialsFactory::TPtr credentialsFactory,
    TDuration requestTimeout = TDuration::Zero());

// Sends one row (ts, label1, sensor1) to the sink.
void WriteOneRow(TFakeCASetup& caSetup);

// {"tok": <structured IAM token>}, for settings whose token name is "tok".
THashMap<TString, TString> IamTokenSecureParams(const TString& token);

// A body the sink accepts as a successful push (CT_SOLOMON / CT_MONIUM and CT_MONITORING field names).
TString SolomonPushOkBody();

// H3: an NHttp TLS server inside the test actor runtime (the sink's own transport on both ends).
// It listens on a free port with the given H2 certificate, records every request to `path`
// (URL, Authorization, raw headers, body) and answers it with the responder's status and body.
struct TNHttpServerRequest {
    TString Url;
    TString Authorization;
    TString Headers;
    TString Body;
};

class TNHttpTlsServer {
public:
    using TResponder = std::function<std::pair<TString, TString>(const TNHttpServerRequest&)>; // status, body

    TNHttpTlsServer(NActors::TTestActorRuntimeBase& runtime, const NYql::NTransportTest::TTestCert& cert, const TString& path = "/api/v2/push");

    // Default: 200 with SolomonPushOkBody().
    void SetResponder(TResponder responder);

    ui16 Port() const {
        return Port_;
    }
    // "localhost:<port>": a name covered by the H2 leaf certificates.
    TString Endpoint() const;

    size_t RequestCount() const;
    TVector<TNHttpServerRequest> Requests() const;

    struct TState;

private:
    std::shared_ptr<TState> State;
    ui16 Port_ = 0;
};

// Actor-runtime log capture: pass CreateCapturingLogBackend(log) to TFakeCASetup. Records also go to stderr.
class TCapturedLog {
public:
    void Append(TStringBuf data);
    TString Text() const;
    bool Contains(TStringBuf needle) const;

private:
    mutable std::mutex Mutex;
    TString Text_;
};

TAutoPtr<TLogBackend> CreateCapturingLogBackend(std::shared_ptr<TCapturedLog> log);

} // namespace NYql::NDq
