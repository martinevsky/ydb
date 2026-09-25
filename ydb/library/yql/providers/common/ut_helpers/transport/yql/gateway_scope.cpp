#include "gateway_scope.h"

#include <ydb/library/yql/providers/common/ut_helpers/transport/wait.h>

#include <util/generic/yexception.h>

namespace NYql::NTransportTest {

TGatewayScope::TGatewayScope(const THttpGatewayConfig& config, const TFactory& factory)
    : Config_(config)
    , Counters_(MakeIntrusive<::NMonitoring::TDynamicCounters>())
    , Inspector_(Counters_)
    , Gateway_(factory ? factory(&Config_, Counters_) : IHTTPGateway::Make(&Config_, Counters_))
{
    // A freshly constructed THTTPMultiGateway creates its gauges on the counters it was given and sets
    // MaxInFlight from the config (default 1024). A reused singleton never touches these counters.
    const i64 expected = Config_.HasMaxInFlightCount() ? Config_.GetMaxInFlightCount() : 1024;
    const auto actual = Inspector_.Find("MaxInFlight");
    if (!actual || *actual != expected) {
        Gateway_.reset();
        ythrow yexception() << "TGatewayScope: IHTTPGateway::Make returned an existing gateway (singleton leaked from "
            << "another test or holder); MaxInFlight on the fresh counters is "
            << (actual ? ToString(*actual) : TString("absent")) << ", expected " << expected;
    }
}

TGatewayScope::~TGatewayScope() {
    try {
        Close();
    } catch (const std::exception& e) {
        Cerr << "TGatewayScope: " << e.what() << Endl;
    }
}

const IHTTPGateway::TPtr& TGatewayScope::Gateway() const {
    Y_ENSURE(Gateway_, "TGatewayScope is closed");
    return Gateway_;
}

void TGatewayScope::Close() {
    if (!Gateway_) {
        return;
    }
    const IHTTPGateway::TWeakPtr weak = Gateway_;
    Gateway_.reset();
    WaitUntil([&]() {
        return weak.expired();
    }, TDuration::Seconds(5), "the HTTP gateway singleton is destroyed (someone else still holds a reference)");
}

} // namespace NYql::NTransportTest
