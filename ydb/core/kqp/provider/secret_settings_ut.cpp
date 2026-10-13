#include "yql_kikimr_provider_impl.h"

#include <library/cpp/testing/unittest/registar.h>

namespace NYql {
namespace {

TWriteSecretSettings ParseSettings(TExprContext& ctx, std::initializer_list<std::pair<TString, TString>> settings) {
    TExprNode::TListType options;
    for (const auto& [name, value] : settings) {
        options.push_back(ctx.NewList(TPositionHandle(), {
            ctx.NewAtom(TPositionHandle(), name), ctx.NewAtom(TPositionHandle(), value),
        }));
    }
    return ParseSecretSettings(NNodes::TExprList(ctx.NewList(TPositionHandle(), std::move(options))), ctx);
}

} // namespace

Y_UNIT_TEST_SUITE(SecretSettings) {
    Y_UNIT_TEST(CreateDelegationPassesOnlySourceParameters) {
        TExprContext ctx;
        const auto settings = ParseSettings(ctx, {
            {"mode", "create"}, {"source", "yc_iam_delegation"},
            {"service_account_id", "sa-1"}, {"resource", "cloud-1"},
        });
        UNIT_ASSERT_C(!settings.HasError, ctx.IssueManager.GetIssues().ToString());
        UNIT_ASSERT_VALUES_EQUAL(settings.Type.Cast().Value(), "YC_IAM_DELEGATION");
        UNIT_ASSERT_VALUES_EQUAL(settings.ServiceAccountId.Cast().Value(), "sa-1");
        UNIT_ASSERT_VALUES_EQUAL(settings.CloudId.Cast().Value(), "cloud-1");
        UNIT_ASSERT(!settings.Value);
        UNIT_ASSERT(!settings.ValueParamName);
    }

    Y_UNIT_TEST(AlterInfersDelegationSourceFromResource) {
        TExprContext ctx;
        const auto settings = ParseSettings(ctx, {{"mode", "alter"}, {"resource", "cloud-2"}});
        UNIT_ASSERT_C(!settings.HasError, ctx.IssueManager.GetIssues().ToString());
        UNIT_ASSERT_VALUES_EQUAL(settings.Type.Cast().Value(), "YC_IAM_DELEGATION");
        UNIT_ASSERT(!settings.ServiceAccountId);
        UNIT_ASSERT(!settings.Value);
    }

    Y_UNIT_TEST(RejectDuplicateAndUnsupportedOptions) {
        for (const auto& option : {"source", "service_account_id", "resource", "unsupported"}) {
            TExprContext ctx;
            const auto settings = ParseSettings(ctx, {
                {"mode", "create"}, {"source", "YC_IAM_DELEGATION"},
                {"service_account_id", "sa-1"}, {"resource", "cloud-1"},
                {option, "extra"},
            });
            UNIT_ASSERT_C(settings.HasError, option);
            UNIT_ASSERT(!ctx.IssueManager.GetIssues().Empty());
        }
    }

    Y_UNIT_TEST(RejectLegacySourceAndMixedValue) {
        {
            TExprContext ctx;
            UNIT_ASSERT(ParseSettings(ctx, {
                {"mode", "create"}, {"source", "IAM_DELEGATION"}, {"service_account_id", "sa-1"},
            }).HasError);
        }
        {
            TExprContext ctx;
            UNIT_ASSERT(ParseSettings(ctx, {
                {"mode", "create"}, {"source", "YC_IAM_DELEGATION"},
                {"service_account_id", "sa-1"}, {"value", "stored-value"},
            }).HasError);
        }
        {
            TExprContext ctx;
            UNIT_ASSERT(ParseSettings(ctx, {
                {"mode", "alter"}, {"resource", "cloud-2"}, {"value", "stored-value"},
            }).HasError);
        }
    }
}

} // namespace NYql
