#pragma once

// Test-only hooks for THTTPMultiGateway (FQ transport test plan, seams S1, S3 and S11).
//
// Production code must keep using IHTTPGateway::Make. Every hook left empty means "exactly today's
// behaviour" (wall clock, the system DNS resolver). The struct is meant to grow: add a field per seam.

#include "yql_http_gateway.h"

#include <util/datetime/base.h>
#include <util/network/address.h>

#include <functional>
#include <vector>

namespace NYql::NHttpGatewayTest {

// S3: the libcurl multi call of the worker loop that TGatewayTestOptions.CurlMulti wraps.
enum class ECurlMultiCall {
    Perform, // curl_multi_perform
    Poll,    // curl_multi_poll
};

struct TGatewayTestOptions {
    // S1: the clock used to sign AWS SigV4 requests and to schedule/compare retry deadlines
    // (the Delayed queue). Empty: TInstant::Now().
    std::function<TInstant()> Now;
    // S11: resolver for THttpGatewayConfig.DnsResolverConfig.ExplicitDNSRecord hosts. Empty: the system
    // resolver. Throw TNetworkResolutionError to simulate a resolution failure.
    std::function<std::vector<NAddr::TOpaqueAddr>(const TString& host, ui16 port)> DnsResolve;
    // S3: wraps curl_multi_perform / curl_multi_poll in the worker loop (THTTPMultiGateway::Perform) to
    // inject CURLM failures. Runs on the curl thread. `call` performs the real libcurl call; return its
    // result, or any other CURLMcode (with or without calling it) to make the loop see that code.
    // Empty: the libcurl functions are called directly.
    std::function<CURLMcode(ECurlMultiCall which, const std::function<CURLMcode()>& call)> CurlMulti;
};

// Same singleton semantics as IHTTPGateway::Make: if a gateway is alive, it is returned as is and the
// arguments (options included) are ignored.
IHTTPGateway::TPtr MakeHttpGatewayForTest(
    const THttpGatewayConfig* httpGatewaysCfg,
    ::NMonitoring::TDynamicCounterPtr counters,
    const TGatewayTestOptions& options);

// S11: re-resolves the explicit DNS records now (instead of waiting for DnsResolverConfig.RefreshMs).
// Requests created afterwards use the new table; requests already created keep theirs.
// Throws if `gateway` is not the curl gateway.
void RefreshDnsNow(const IHTTPGateway::TPtr& gateway);

} // namespace NYql::NHttpGatewayTest
