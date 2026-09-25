#include <ydb/core/fq/libs/test_connection/events/events.h>
#include <ydb/core/fq/libs/test_connection/test_connection.h>

#include <ydb/core/testlib/actors/test_runtime.h>
#include <ydb/core/testlib/basics/helpers.h>
#include <ydb/library/yql/providers/common/token_accessor/client/factory.h>

#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/testing/unittest/tests_data.h>

namespace NFq {

Y_UNIT_TEST_SUITE(TestMonitoringConnection) {
    Y_UNIT_TEST(UnreachableEndpointIsAnError) {
        // Nothing listens on the port: the HTTP proxy answers with an error and no
        // response at all, which must become an issue, not a null dereference.
        NKikimr::TTestActorRuntime runtime(1, true);
        TAutoPtr<NKikimr::TAppPrepare> app = new NKikimr::TAppPrepare();
        runtime.Initialize(app->Unwrap());

        TPortManager portManager;
        const TString endpoint = "localhost:" + ToString(portManager.GetTcpPort());

        FederatedQuery::Monitoring monitoring;
        monitoring.set_project("project");
        monitoring.set_cluster("cluster");
        monitoring.mutable_auth()->mutable_none();

        auto counters = MakeIntrusive<TTestConnectionRequestCounters>("TestMonitoringConnection");
        counters->Register(MakeIntrusive<::NMonitoring::TDynamicCounters>());

        const NActors::TActorId sender = runtime.AllocateEdgeActor();
        runtime.Register(CreateTestMonitoringConnectionActor(
            monitoring, sender, 0, endpoint, NYql::CreateStructuredTokenCredentialsFactory(),
            "scope", "user", "", nullptr, counters));

        TAutoPtr<NActors::IEventHandle> handle;
        auto* response = runtime.GrabEdgeEvent<TEvTestConnection::TEvTestConnectionResponse>(handle, TDuration::Seconds(60));
        UNIT_ASSERT_C(response, "no answer from the test connection actor");
        UNIT_ASSERT_C(response->Issues, "an unreachable endpoint was reported as a working connection");
        UNIT_ASSERT_STRING_CONTAINS(response->Issues.ToString(), "Monitoring");
    }
}

} // namespace NFq
