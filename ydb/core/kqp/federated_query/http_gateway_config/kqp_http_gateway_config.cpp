#include "kqp_http_gateway_config.h"

#include <ydb/core/protos/config.pb.h>

namespace NKikimr::NKqp {

    NYql::THttpGatewayConfig DefaultHttpGatewayConfig() {
        NYql::THttpGatewayConfig config;
        config.SetMaxInFlightCount(2000);
        config.SetMaxSimulatenousDownloadsSize(2000000000);
        config.SetBuffersSizePerStream(5000000);
        config.SetConnectionTimeoutSeconds(15);
        config.SetRequestTimeoutSeconds(0);
        return config;
    }

    NYql::THttpGatewayConfig GetEffectiveHttpGatewayConfig(const NKikimrConfig::TQueryServiceConfig& queryServiceConfig) {
        return queryServiceConfig.HasHttpGateway() ? queryServiceConfig.GetHttpGateway() : DefaultHttpGatewayConfig();
    }

    ui64 PoolCapMaxHandlers(const NKikimrConfig::TQueryServiceConfig& queryServiceConfig) {
        const auto& httpGatewayConfig = queryServiceConfig.GetHttpGateway();
        return httpGatewayConfig.HasMaxInFlightCount() ? httpGatewayConfig.GetMaxInFlightCount() : 1024;
    }

}  // namespace NKikimr::NKqp
