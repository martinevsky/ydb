#pragma once

// H11: TGatewayScope, singleton hygiene for IHTTPGateway tests.
//
// IHTTPGateway::Make returns the live process-wide gateway if one exists and silently ignores the new
// config and counters (F-A-2). A test that inherits another test's gateway tests the wrong config.
// TGatewayScope makes the gateway with its own fresh counters and throws if Make returned an existing
// gateway (detected through the MaxInFlight gauge, which only a freshly constructed gateway sets on
// these counters). Close() drops the scope's reference and throws if anything else still holds the
// gateway (so the next test would inherit it). The destructor calls Close() and reports (not throws)
// a failure to Cerr.
//
// Do not link http_gateway/ut_helpers (THttpGatewayHolder) into a target using this: it pins the
// singleton for the whole process.

#include <ydb/library/yql/providers/common/ut_helpers/transport/counters_inspector.h>

#include <ydb/library/yql/providers/common/http_gateway/yql_http_gateway.h>
#include <yql/essentials/providers/common/proto/gateways_config.pb.h>

#include <functional>

namespace NYql::NTransportTest {

class TGatewayScope {
public:
    // Makes the gateway; IHTTPGateway::Make by default. Tests that need seams pass e.g.
    // NHttpGatewayTest::MakeHttpGatewayForTest bound to their TGatewayTestOptions.
    using TFactory = std::function<IHTTPGateway::TPtr(const THttpGatewayConfig*, ::NMonitoring::TDynamicCounterPtr)>;

    explicit TGatewayScope(const THttpGatewayConfig& config = {}, const TFactory& factory = {});
    ~TGatewayScope();

    TGatewayScope(const TGatewayScope&) = delete;
    TGatewayScope& operator=(const TGatewayScope&) = delete;

    const IHTTPGateway::TPtr& Gateway() const;
    IHTTPGateway* operator->() const {
        return Gateway().get();
    }
    const ::NMonitoring::TDynamicCounterPtr& Counters() const {
        return Counters_;
    }
    const TCountersInspector& Inspector() const {
        return Inspector_;
    }
    const THttpGatewayConfig& Config() const {
        return Config_;
    }

    // Releases the gateway and verifies it was destroyed. Idempotent. Throws yexception on a leak.
    void Close();

private:
    const THttpGatewayConfig Config_;
    const ::NMonitoring::TDynamicCounterPtr Counters_;
    const TCountersInspector Inspector_;
    IHTTPGateway::TPtr Gateway_;
};

} // namespace NYql::NTransportTest
