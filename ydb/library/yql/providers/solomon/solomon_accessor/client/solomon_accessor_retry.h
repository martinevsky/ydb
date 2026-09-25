#pragma once

#include <contrib/libs/curl/include/curl/curl.h>
#include <library/cpp/retry/retry_policy.h>

namespace NYql::NSo {

// Retry class of a Solomon/Monium HTTP API call made through the curl gateway.
// Transient transport errors (connection reset, timeouts, partial transfers, DNS
// hiccups, ...) and the HTTP codes in NConstants::RetriableHttpCodes are retried;
// everything else (deterministic configuration errors, other HTTP codes) is not.
ERetryErrorClass SolomonHttpRetryClass(CURLcode curlCode, long httpCode);

} // namespace NYql::NSo
