#include "fake_data_service.h"

#include <ydb/library/yql/providers/common/ut_helpers/transport/wait.h>

#include <util/generic/yexception.h>
#include <util/string/ascii.h>
#include <util/string/builder.h>

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

namespace NYql::NTransportTest {

using namespace yandex::cloud::priv::monitoring::v3;

namespace {

enum class EMode {
    Ok,
    Status,
    EchoMetadata,
};

void FillPoints(ReadResponse* response, ui64 points) {
    auto* timeseries = response->add_response_per_query()->mutable_timeseries_vector()->add_values();
    timeseries->set_name("m");
    timeseries->set_type(MetricType::DGAUGE);
    auto* timestamps = timeseries->mutable_timestamp_values()->mutable_values();
    auto* values = timeseries->mutable_double_values()->mutable_values();
    timestamps->Reserve(points);
    values->Reserve(points);
    for (ui64 i = 0; i < points; ++i) {
        timestamps->Add(static_cast<i64>(1'000'000'000'000LL + i * 1000));
        values->Add(1.5);
    }
}

} // namespace

TMaybe<TString> TDataServiceCall::Metadata(TStringBuf name) const {
    for (const auto& [key, value] : MetadataList) {
        if (AsciiEqualsIgnoreCase(key, name)) {
            return value;
        }
    }
    return Nothing();
}

bool TDataServiceCall::HasMetadata(TStringBuf name) const {
    return Metadata(name).Defined();
}

struct TFakeDataService::TState {
    mutable std::mutex Mutex;
    std::condition_variable Changed;

    EMode Mode = EMode::Ok;
    ui64 Points = 1;
    grpc::StatusCode Code = grpc::StatusCode::OK;
    TString Message;
    bool Hold = false;
    bool Stopping = false;

    TVector<TDataServiceCall> Calls;
    ui64 HeldNow = 0;
    bool ObservedCancelled = false;
};

class TFakeDataService::TService final : public DataService::Service {
public:
    explicit TService(std::shared_ptr<TState> state)
        : State_(std::move(state))
    {}

    grpc::Status Read(grpc::ServerContext* context, const ReadRequest* request, ReadResponse* response) override {
        TDataServiceCall call;
        for (const auto& [key, value] : context->client_metadata()) {
            call.MetadataList.emplace_back(TString(key.data(), key.size()), TString(value.data(), value.size()));
        }
        call.Request = *request;

        std::unique_lock lock(State_->Mutex);
        const ui64 index = State_->Calls.size();
        call.Index = index;
        call.Held = State_->Hold;
        State_->Calls.push_back(call);
        State_->Changed.notify_all();

        if (State_->Hold) {
            ++State_->HeldNow;
            State_->Changed.notify_all();
            bool cancelled = false;
            // Server-side wait for the test's Release(). grpc offers no cancellation callback to a
            // synchronous handler, so IsCancelled() is polled; this is not an oracle.
            while (State_->Hold && !State_->Stopping) {
                if (context->IsCancelled()) {
                    cancelled = true;
                    break;
                }
                State_->Changed.wait_for(lock, std::chrono::milliseconds(1));
            }
            --State_->HeldNow;
            if (cancelled) {
                State_->Calls[index].Cancelled = true;
                State_->ObservedCancelled = true;
                State_->Changed.notify_all();
                return grpc::Status(grpc::StatusCode::CANCELLED, "cancelled by the client while held");
            }
            State_->Changed.notify_all();
        }

        const EMode mode = State_->Mode;
        const ui64 points = State_->Points;
        const grpc::StatusCode code = State_->Code;
        const TString message = State_->Message;
        lock.unlock();

        switch (mode) {
            case EMode::Ok:
                FillPoints(response, points);
                return grpc::Status::OK;
            case EMode::Status:
                return grpc::Status(code, message);
            case EMode::EchoMetadata: {
                TStringBuilder echo;
                for (const auto& [key, value] : call.MetadataList) {
                    echo << key << ": " << value << "\n";
                }
                return grpc::Status(code, echo);
            }
        }
        return grpc::Status(grpc::StatusCode::INTERNAL, "unknown fake mode");
    }

private:
    const std::shared_ptr<TState> State_;
};

TFakeDataService::TFakeDataService(TFakeDataServiceOptions options)
    : State_(std::make_shared<TState>())
    , Service_(std::make_unique<TService>(State_))
{
    std::shared_ptr<grpc::ServerCredentials> credentials;
    if (options.Tls) {
        grpc::SslServerCredentialsOptions sslOptions;
        sslOptions.pem_key_cert_pairs.push_back({options.Tls->KeyPem, options.Tls->CertPem});
        credentials = grpc::SslServerCredentials(sslOptions);
    } else {
        credentials = grpc::InsecureServerCredentials();
    }

    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", credentials, &Port_);
    builder.RegisterService(Service_.get());
    builder.SetMaxSendMessageSize(-1);
    builder.SetMaxReceiveMessageSize(-1);
    Server_ = builder.BuildAndStart();
    Y_ENSURE(Server_ && Port_ > 0, "TFakeDataService: failed to start the gRPC server on 127.0.0.1:0");
}

TFakeDataService::~TFakeDataService() {
    Shutdown();
}

ui16 TFakeDataService::Port() const {
    return static_cast<ui16>(Port_);
}

TString TFakeDataService::Endpoint(TStringBuf host) const {
    return TStringBuilder() << host << ":" << Port_;
}

void TFakeDataService::ReturnOk(ui64 points) {
    std::lock_guard lock(State_->Mutex);
    State_->Mode = EMode::Ok;
    State_->Points = points;
}

void TFakeDataService::ReturnStatus(grpc::StatusCode code, TString message) {
    Y_ENSURE(code != grpc::StatusCode::OK, "ReturnStatus needs a non-OK code; use ReturnOk");
    std::lock_guard lock(State_->Mutex);
    State_->Mode = EMode::Status;
    State_->Code = code;
    State_->Message = std::move(message);
}

void TFakeDataService::Oversized(ui64 points) {
    ReturnOk(points);
}

void TFakeDataService::EchoMetadataInMessage(grpc::StatusCode code) {
    Y_ENSURE(code != grpc::StatusCode::OK, "EchoMetadataInMessage needs a non-OK code");
    std::lock_guard lock(State_->Mutex);
    State_->Mode = EMode::EchoMetadata;
    State_->Code = code;
}

void TFakeDataService::HoldUntilRelease() {
    std::lock_guard lock(State_->Mutex);
    State_->Hold = true;
}

void TFakeDataService::Release() {
    std::lock_guard lock(State_->Mutex);
    State_->Hold = false;
    State_->Changed.notify_all();
}

TVector<TDataServiceCall> TFakeDataService::Calls() const {
    std::lock_guard lock(State_->Mutex);
    return State_->Calls;
}

ui64 TFakeDataService::CallCount() const {
    std::lock_guard lock(State_->Mutex);
    return State_->Calls.size();
}

ui64 TFakeDataService::HeldNow() const {
    std::lock_guard lock(State_->Mutex);
    return State_->HeldNow;
}

bool TFakeDataService::ObservedCancelled() const {
    std::lock_guard lock(State_->Mutex);
    return State_->ObservedCancelled;
}

void TFakeDataService::WaitForCalls(ui64 count, TDuration guard) const {
    WaitUntil([&]() {
        return CallCount() >= count;
    }, guard, TStringBuilder() << "TFakeDataService received " << count << " Read call(s)");
}

void TFakeDataService::WaitForHeldCalls(ui64 count, TDuration guard) const {
    WaitUntil([&]() {
        return HeldNow() >= count;
    }, guard, TStringBuilder() << "TFakeDataService holds " << count << " Read call(s)");
}

void TFakeDataService::WaitObservedCancelled(TDuration guard) const {
    WaitUntil([&]() {
        return ObservedCancelled();
    }, guard, "TFakeDataService observed a held Read cancelled by the client");
}

void TFakeDataService::Shutdown() {
    if (!Server_) {
        return;
    }
    {
        std::lock_guard lock(State_->Mutex);
        State_->Stopping = true;
        State_->Changed.notify_all();
    }
    Server_->Shutdown();
    Server_->Wait();
    Server_.reset();
}

} // namespace NYql::NTransportTest
