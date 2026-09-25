#pragma once

#include <ydb/library/actors/http/http_proxy.h>

#include <library/cpp/retry/retry_policy.h>

namespace NYql::NSo {

// Retry class of one Solomon sink push attempt, as used by the sink's http sender retry policy.
// `response` is null or has no Response when the connection was not established.
ERetryErrorClass SinkHttpRetryClass(const NHttp::TEvHttpProxy::TEvHttpIncomingResponse* response);

} // namespace NYql::NSo
