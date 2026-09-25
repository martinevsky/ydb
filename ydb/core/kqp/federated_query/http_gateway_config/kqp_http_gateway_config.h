#pragma once

#include <yql/essentials/providers/common/proto/gateways_config.pb.h>

#include <util/system/types.h>

namespace NKikimrConfig {
    class TQueryServiceConfig;
}  // namespace NKikimrConfig

namespace NKikimr::NKqp {

    // HTTP gateway config of a KQP node whose QueryServiceConfig has no HttpGateway section.
    NYql::THttpGatewayConfig DefaultHttpGatewayConfig();

    // The config TKqpFederatedQuerySetupFactoryDefault passes to the HTTP gateway: the HttpGateway
    // section as is when present (no field-level merge), DefaultHttpGatewayConfig() otherwise.
    NYql::THttpGatewayConfig GetEffectiveHttpGatewayConfig(const NKikimrConfig::TQueryServiceConfig& queryServiceConfig);

    // maxHandlers of the HTTP pool-cap pusher started by the KQP proxy: MaxInFlightCount of the
    // HttpGateway section when it is set there, 1024 otherwise. It reads the section, not the
    // effective config.
    ui64 PoolCapMaxHandlers(const NKikimrConfig::TQueryServiceConfig& queryServiceConfig);

}  // namespace NKikimr::NKqp
