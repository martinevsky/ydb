#pragma once

// H4: TFakeDataService, an in-process gRPC Monitoring DataService (the Solomon/Monium read API)
// on 127.0.0.1:<port 0>, plaintext or TLS. Framework-neutral: failures are reported by throwing
// yexception (see ydb/library/yql/providers/common/ut_helpers/transport/README.md).
//
//   TFakeDataService service;                              // or ({.Tls = pki.Leaf()})
//   service.ReturnStatus(grpc::StatusCode::INTERNAL, "x"); // every Read answers with this status
//   service.HoldUntilRelease();                            // Read blocks until Release()
//   service.WaitForHeldCalls(1);                           // sync point: a Read is being held
//   service.ObservedCancelled();                           // a held Read saw IsCancelled()
//   service.Calls()[0].Metadata("authorization");          // client metadata of each Read
//
// Modes (the last one set wins; the default answers OK with one small timeseries):
//   ReturnOk(points)          OK with one timeseries "m" of `points` points
//   ReturnStatus(code, msg)   a non-OK status
//   Oversized(points)         OK with `points` points: 5M points is about 70 MB, above the client's
//                             default 64 MB receive limit (the server's send limit is unlimited)
//   EchoMetadataInMessage(c)  status `c` whose message is the received client metadata
// HoldUntilRelease() is orthogonal: a held Read waits (not replying) until Release(), server
// shutdown, or the client cancelling the call (deadline, channel shutdown); a cancelled held Read
// sets ObservedCancelled() and answers CANCELLED.
//
// D-7 (test_plan.md §6.3): shut this server down (destructor or Shutdown()) before the last
// reference to the curl HTTP gateway is released.

#include <ydb/library/yql/providers/common/ut_helpers/transport/test_pki.h>

#include <ydb/library/yql/providers/solomon/solomon_accessor/grpc/data_service.grpc.pb.h>

#include <util/datetime/base.h>
#include <util/generic/maybe.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>

#include <grpcpp/server.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>

namespace NYql::NTransportTest {

struct TFakeDataServiceOptions {
    // TLS mode: the server presents this certificate (CertPem + KeyPem).
    std::optional<TTestCert> Tls;
};

struct TDataServiceCall {
    ui64 Index = 0;
    // Client metadata in arrival order; names are lowercase on the wire.
    TVector<std::pair<TString, TString>> MetadataList;
    yandex::cloud::priv::monitoring::v3::ReadRequest Request;
    bool Held = false;      // the call was held by HoldUntilRelease()
    bool Cancelled = false; // IsCancelled() was observed while held

    // First metadata value with this (lowercase) name.
    TMaybe<TString> Metadata(TStringBuf name) const;
    bool HasMetadata(TStringBuf name) const;
};

class TFakeDataService {
public:
    explicit TFakeDataService(TFakeDataServiceOptions options = {});
    ~TFakeDataService();

    TFakeDataService(const TFakeDataService&) = delete;
    TFakeDataService& operator=(const TFakeDataService&) = delete;

    ui16 Port() const;
    // "<host>:<port>" for TDqSolomonSource::GrpcEndpoint.
    TString Endpoint(TStringBuf host = "127.0.0.1") const;

    void ReturnOk(ui64 points = 1);
    void ReturnStatus(grpc::StatusCode code, TString message);
    void Oversized(ui64 points);
    void EchoMetadataInMessage(grpc::StatusCode code);

    void HoldUntilRelease();
    void Release();

    TVector<TDataServiceCall> Calls() const;
    ui64 CallCount() const;
    ui64 HeldNow() const;
    bool ObservedCancelled() const;

    // Guarded waits (throw yexception on guard expiry).
    void WaitForCalls(ui64 count, TDuration guard = TDuration::Seconds(10)) const;
    void WaitForHeldCalls(ui64 count, TDuration guard = TDuration::Seconds(10)) const;
    void WaitObservedCancelled(TDuration guard = TDuration::Seconds(10)) const;

    // Releases held calls, then shuts the server down and waits for it. Idempotent.
    void Shutdown();

private:
    class TService;
    struct TState;

    const std::shared_ptr<TState> State_;
    std::unique_ptr<TService> Service_;
    std::unique_ptr<grpc::Server> Server_;
    int Port_ = 0;
};

} // namespace NYql::NTransportTest
