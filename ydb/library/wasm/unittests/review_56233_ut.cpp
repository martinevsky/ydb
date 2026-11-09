#include <ydb/library/wasm/api/compartment.h>
#include <ydb/library/wasm/api/function.h>
#include <ydb/library/wasm/engine/wavm_private_imports.h>

#include <library/cpp/testing/gtest/gtest.h>
#include <util/generic/scope.h>
#include <util/generic/yexception.h>

#if defined(_unix_)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace NYdb::NWasm {
namespace {

struct TReproTag {};

void Load(IWebAssemblyCompartment* compartment, const TString& source, bool precompiled)
{
    if (!precompiled) {
        compartment->AddModule(source);
        return;
    }
    WAVM::IR::Module module;
    module.featureSpec.memory64 = true;
    module.featureSpec.table64 = true;
    std::vector<WAVM::WAST::Error> errors;
    ASSERT_TRUE(WAVM::WAST::parseModule(source.c_str(), source.size() + 1, module, errors));
    const auto binary = WAVM::WASM::saveBinaryModule(module);
    const auto compiled = WAVM::Runtime::compileModule(module);
    const auto object = WAVM::Runtime::getObjectCode(compiled);
    compartment->AddPrecompiledModule({
        EBytecodeFormat::Binary, TSharedRef::MakeCopy<TReproTag>(TRef(binary.data(), binary.size())),
        TSharedRef::MakeCopy<TReproTag>(TRef(object.data(), object.size())),
    });
}

ui64 Invoke(IWebAssemblyCompartment* compartment)
{
    auto* previous = GetCurrentCompartment();
    SetCurrentCompartment(compartment);
    Y_DEFER { SetCurrentCompartment(previous); };
    try {
        return TCompartmentFunction<ui64()>(compartment, "invoke")();
    } catch (WAVM::Runtime::Exception* ex) {
        const auto description = WAVM::Runtime::describeException(ex);
        WAVM::Runtime::destroyException(ex);
        ythrow yexception() << description;
    }
}

const TString Consumer = R"((module
    (type $signature (func (result i64)))
    (import "env" "__indirect_function_table" (table i64 1 funcref))
    (import "GOT.func" "f" (global $address (mut i64)))
    (func (export "invoke") (result i64)
        (call_indirect (type $signature) (global.get $address)))
))";

TEST(TWasmReview56233, PrivateTableDoesNotSupplySharedGotSlot)
{
    for (bool precompiled : {false, true}) {
        SCOPED_TRACE(precompiled);
        auto compartment = CreateEmptyImage();
        Load(compartment.get(), R"((module
            (table i64 8 funcref)
            (func $different_debug_name (export "f") (result i64) (i64.const 1))
            (elem (i64.const 7) func $different_debug_name)
        ))", precompiled);
        Load(compartment.get(), Consumer, precompiled);
        EXPECT_NO_THROW({ EXPECT_EQ(Invoke(compartment.get()), 1u); });
    }
}

TEST(TWasmReview56233, OverwrittenElementIsNotReusedForExport)
{
    for (bool precompiled : {false, true}) {
        SCOPED_TRACE(precompiled);
        auto compartment = CreateEmptyImage();
        Load(compartment.get(), R"((module
            (import "env" "__indirect_function_table" (table i64 1 funcref))
            (func $different_debug_name (export "f") (result i64) (i64.const 1))
            (func $g (result i64) (i64.const 2))
            (elem (i64.const 7) func $different_debug_name)
            (elem (i64.const 7) func $g)
        ))", precompiled);
        Load(compartment.get(), Consumer, precompiled);
        EXPECT_NO_THROW({ EXPECT_EQ(Invoke(compartment.get()), 1u); });
    }
}

#if defined(_unix_)
TEST(TWasmReview56233, StartCannotBlockCompartmentAcquisitionForever)
{
    const auto child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        alarm(15);
        try {
            auto compartment = CreateEmptyImage();
            compartment->AddModule(TString(R"((module
                (func $start (loop $forever (br $forever)))
                (start $start)
                (func (export "returns") (result i64) (i64.const 1))
            ))"));
            _exit(0);
        } catch (const std::exception& ex) {
            // A future initialization budget may terminate the loop with this trap.
            _exit(TStringBuf(ex.what()).Contains("timeoutExpired") ? 0 : 2);
        } catch (...) {
            _exit(2);
        }
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status)) << "Loading the module did not finish before the subprocess alarm; status=" << status;
    EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif

} // namespace
} // namespace NYdb::NWasm
