#include <ydb/library/yql/providers/common/ut_helpers/async_credentials.h>
#include <ydb/library/yql/providers/s3/actors/yql_s3_read_actor.h>
#include <ydb/library/yql/providers/s3/events/events.h>
#include <ydb/library/yql/providers/s3/proto/range.pb.h>
#include <ydb/library/yql/dq/actors/compute/dq_compute_actor.h>
#include <ydb/library/yql/providers/common/ut_helpers/dq_fake_ca.h>
#include <ydb/library/services/services.pb.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/Formats/registerFormats.h>

#include <contrib/libs/apache/arrow/cpp/src/arrow/api.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/io/memory.h>
#include <contrib/libs/apache/arrow/cpp/src/parquet/arrow/schema.h>
#include <contrib/libs/apache/arrow/cpp/src/parquet/arrow/writer.h>
#include <contrib/libs/apache/arrow/cpp/src/parquet/metadata.h>

#include <library/cpp/testing/unittest/registar.h>
#include <util/generic/size_literals.h>
#include <util/generic/scope.h>
#include <util/stream/str.h>

#include <algorithm>

namespace NYql::NDq {
namespace {

using namespace NActors;
using namespace NKikimr::NMiniKQL;

// Serve the file through the same ranged HTTP interface as the production reader.
class TParquetGateway : public IHTTPGateway {
public:
    TString Body;
    bool HoldChunks = false;
    ui32 RangeCalls = 0;
    TVector<TString> Tokens;
    std::function<void()> OnRange;
    std::vector<std::function<void()>> PendingChunks;

    void Upload(TString, THeaders, TString, TOnResult, bool, TRetryPolicy::TPtr, IHttpRequestContext::TPtr) override {
        UNIT_FAIL("unexpected upload");
    }

    void Delete(TString, THeaders, TOnResult, TRetryPolicy::TPtr, IHttpRequestContext::TPtr) override {
        UNIT_FAIL("unexpected delete");
    }

    void Download(TString, THeaders headers, size_t offset, size_t size, TOnResult callback, TString, TRetryPolicy::TPtr, IHttpRequestContext::TPtr) override {
        ++RangeCalls;
        for (const auto& header : headers.Fields) {
            constexpr TStringBuf prefix = "X-YaCloud-SubjectToken:";
            if (header.StartsWith(prefix)) {
                Tokens.emplace_back(header.substr(prefix.size()));
            }
        }
        if (OnRange) {
            OnRange();
        }
        UNIT_ASSERT_C(offset <= Body.size() && size <= Body.size() - offset, "invalid HTTP range " << offset << "+" << size);
        auto reply = [this, offset, size, callback = std::move(callback)] {
            callback(TResult(TContent(Body.substr(offset, size), 206)));
        };
        // Footer reads include the trailer; column chunks end before it.
        if (HoldChunks && offset + size < Body.size() - 8) {
            PendingChunks.push_back(std::move(reply));
        } else {
            reply();
        }
    }

    TCancelHook Download(TString, THeaders, size_t, size_t, TOnDownloadStart, TOnNewDataPart, TOnDownloadFinish,
        const ::NMonitoring::TDynamicCounters::TCounterPtr&, IHttpRequestContext::TPtr) override {
        UNIT_FAIL("unexpected streaming download");
        return {};
    }

    ui64 GetBuffersSizePerStream() override {
        return 0;
    }

    void UpdatePoolCaps(THashMap<TWorkScope, size_t>) override {}

    void ReleaseChunks() {
        HoldChunks = false;
        auto pending = std::move(PendingChunks);
        for (auto& reply : pending) {
            reply();
        }
    }
};

struct TDecoderError {
    NDqProto::StatusIds::StatusCode Code;
    TString Issues;
};

// Separate mailbox: the parent and fake compute mailbox are blocked in Download.
class TDecoderErrorRecipient : public TActor<TDecoderErrorRecipient> {
public:
    explicit TDecoderErrorRecipient(NThreading::TPromise<TDecoderError> error)
        : TActor(&TDecoderErrorRecipient::StateFunc)
        , Error(std::move(error))
    {}

private:
    STRICT_STFUNC(StateFunc,
        hFunc(IDqComputeActorAsyncInput::TEvAsyncInputError, Handle);
        IgnoreFunc(IDqComputeActorAsyncInput::TEvNewAsyncInputDataArrived);
    )

    void Handle(IDqComputeActorAsyncInput::TEvAsyncInputError::TPtr& ev) {
        Error.TrySetValue({ev->Get()->FatalCode, ev->Get()->Issues.ToOneLineString()});
    }

    NThreading::TPromise<TDecoderError> Error;
};

class TStreamingGateway : public TParquetGateway {
public:
    bool HoldHook = true;
    NThreading::TPromise<void> ReleaseHook = NThreading::NewPromise();
    std::atomic<ui32> StreamCalls{0};
    std::atomic<ui32> CancelCalls{0};
    std::atomic<bool> ReleaseTimedOut{false};

    TCancelHook Download(TString, THeaders, size_t, size_t, TOnDownloadStart onStart,
        TOnNewDataPart onData, TOnDownloadFinish onFinish,
        const ::NMonitoring::TDynamicCounters::TCounterPtr& inflightCounter, IHttpRequestContext::TPtr) override {
        ++StreamCalls;
        onStart(CURLE_OK, 200);
        onData(TCountedContent(TString(Body), std::make_shared<std::atomic_size_t>(0), inflightCounter, {}, 0));
        onFinish(CURLE_OK, {});
        // The test releases this only after a decoder error proves Cancel() ran.
        if (HoldHook) {
            ReleaseTimedOut.store(!ReleaseHook.GetFuture().Wait(TDuration::Seconds(10)));
        }
        return [this](const TIssue&) { ++CancelCalls; };
    }
};

NS3::TSource CsvSource() {
    NS3::TSource source;
    source.SetUrl("http://fake/");
    source.SetFormat("csv_with_names");
    source.SetRowType(R"(["StructType";[["value";["DataType";"Int32"]]]])");
    source.SetAsyncDecoding(true);
    return source;
}

NS3::TSource ParquetSource(ui64 parallelRowGroups = 0, bool selectColumn = true) {
    NS3::TSource source;
    source.SetUrl("http://fake/");
    source.SetFormat("parquet");
    source.SetRowType(selectColumn ? R"(["StructType";[["ts";["DataType";"Timestamp"]]]])" : R"(["StructType";[]])");
    source.SetParallelRowGroupCount(parallelRowGroups);
    return source;
}

std::shared_ptr<arrow::Schema> ParquetSchema() {
    return arrow::schema({arrow::field("ts", arrow::timestamp(arrow::TimeUnit::MICRO), false)});
}

// Valid control: one timestamp and one row per row group.
TString MakeParquet(int rows) {
    arrow::TimestampBuilder builder(arrow::timestamp(arrow::TimeUnit::MICRO), arrow::default_memory_pool());
    for (int i = 0; i < rows; ++i) {
        UNIT_ASSERT(builder.Append(i * 1000000LL).ok());
    }
    std::shared_ptr<arrow::Array> array;
    UNIT_ASSERT(builder.Finish(&array).ok());
    auto table = arrow::Table::Make(ParquetSchema(), {array});
    auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
    UNIT_ASSERT(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), sink, 1).ok());
    auto buffer = sink->Finish().ValueOrDie();
    return TString(reinterpret_cast<const char*>(buffer->data()), buffer->size());
}

// Replacement fixture for corrupt metadata: empty row groups with arbitrary signed sizes.
TString MakeParquetWithCompressedSizes(const std::vector<i64>& sizes) {
    auto properties = parquet::WriterProperties::Builder().build();
    std::shared_ptr<parquet::SchemaDescriptor> schema;
    UNIT_ASSERT(parquet::arrow::ToParquetSchema(ParquetSchema().get(), *properties, &schema).ok());
    auto builder = parquet::FileMetaDataBuilder::Make(schema.get(), properties);
    for (i64 size : sizes) {
        auto rowGroup = builder->AppendRowGroup();
        auto column = rowGroup->NextColumnChunk();
        column->Finish(0, 0, -1, 4, size, 0, false, false, {}, {});
        rowGroup->set_num_rows(0);
        rowGroup->Finish(0);
    }
    auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
    builder->Finish()->WriteTo(sink.get());
    auto footer = sink->Finish().ValueOrDie();
    TString body = "PAR1";
    body.append(reinterpret_cast<const char*>(footer->data()), footer->size());
    // The Parquet trailer stores the footer length in little-endian order.
    for (ui32 i = 0; i < 4; ++i) {
        body.push_back(static_cast<char>((footer->size() >> (8 * i)) & 0xff));
    }
    return body + "PAR1";
}

struct TReadResult {
    bool Finished = false;
    ui64 Rows = 0;
    ui64 DeliveredRows = 0;
    std::vector<i64> Values;
    std::vector<NDqProto::StatusIds::StatusCode> Errors;
    TString Issues;
};

// All callbacks and observations run on the simulated actor runtime's single thread.
class TParquetReader {
public:
    TReadResult Result;

    TParquetReader() {
        Runtime.AddLocalService(FakeActorId, TActorSetupCmd(new TFakeActor(
            std::make_shared<TAsyncInputPromises>(), std::make_shared<TAsyncOutputPromises>()), TMailboxType::Simple, 0));
        Runtime.Initialize();
        Runtime.GetLogSettings(0)->Append(NKikimrServices::EServiceKikimr_MIN,
            NKikimrServices::EServiceKikimr_MAX, NKikimrServices::EServiceKikimr_Name);
        Runtime.SetRegistrationObserverFunc([this](TTestActorRuntimeBase& runtime, const TActorId& parent, const TActorId& actor) {
            TTestActorRuntimeBase::DefaultRegistrationObserver(runtime, parent, actor);
            if (runtime.FindActorName(actor) == "S3_READ") {
                DecoderId = actor;
                ReaderId = parent;
            }
        });
        Runtime.SetObserverFunc([this](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvS3Provider::TEvFileFinished::EventType && ev->Sender == DecoderId) {
                DecoderFinished = true;
            }
            if (ev->GetTypeRewrite() == TEvS3Provider::TEvRetryEventFunc::EventType) {
                ++CredentialContinuations;
            }
            if (ev->GetTypeRewrite() == IDqComputeActorAsyncInput::TEvAsyncInputError::EventType) {
                auto* error = ev->Get<IDqComputeActorAsyncInput::TEvAsyncInputError>();
                Result.Errors.push_back(error->FatalCode);
                Result.Issues += error->Issues.ToOneLineString();
            } else if (ev->GetTypeRewrite() == TEvS3Provider::TEvNextRecordBatch::EventType) {
                const auto& batch = ev->Get<TEvS3Provider::TEvNextRecordBatch>()->Batch;
                Result.Rows += batch->num_rows();
                if (batch->num_columns()) {
                    auto values = std::static_pointer_cast<arrow::TimestampArray>(batch->column(0));
                    for (i64 i = 0; i < values->length(); ++i) {
                        Result.Values.push_back(values->Value(i));
                    }
                }
            }
            return TTestActorRuntimeBase::EEventAction::PROCESS;
        });
    }

    ~TParquetReader() {
        Execute([](TFakeActor& actor) { actor.Terminate(); });
    }

    void Start(std::shared_ptr<TParquetGateway> gateway, NS3::TSource source, ui64 dataInflight = 200_MB,
        IStructuredTokenCredentialsFactory::TPtr credentials = CreateStructuredTokenCredentialsFactory()) {
        NS3::TRange range;
        auto* path = range.AddPaths();
        path->SetName("file.parquet");
        path->SetSize(gateway->Body.size());
        path->SetRead(true);
        TStringStream encodedRange;
        range.Save(&encodedRange);
        Execute([&](TFakeActor& actor) {
            TS3ReadActorFactoryConfig config;
            config.DataInflight = dataInflight;
            auto [input, inputActor] = CreateS3ReadActor(actor.TypeEnv, actor.HolderFactory,
                std::shared_ptr<TScopedAlloc>(&actor.Alloc, [](TScopedAlloc*) {}), gateway, std::move(source),
                0, TCollectStatsLevel::None, TTxId{}, {}, {}, {encodedRange.Str()}, FakeActorId,
                credentials, GetHTTPDefaultRetryPolicy(), config, nullptr, nullptr,
                std::make_shared<TGuaranteeQuotaManager>(dataInflight, dataInflight), false);
            actor.InitAsyncInput(input, inputActor);
        });
    }

    void Pump() {
        Runtime.SimulateSleep(TDuration::MilliSeconds(1));
        Execute([&](TFakeActor& actor) {
            TMaybe<TInstant> watermark;
            TUnboxedValueBatch buffer;
            actor.DqAsyncInput->GetAsyncInputData(buffer, watermark, Result.Finished, 1_MB);
            Result.DeliveredRows += buffer.RowCount();
        });
    }

    void CancelDecoder() {
        UNIT_ASSERT_C(DecoderId, "streaming decoder was not registered");
        UNIT_ASSERT_VALUES_EQUAL(CredentialContinuations, 0);
        Runtime.Send(new IEventHandle(DecoderId, FakeActorId, new TEvents::TEvPoison()));
        TDispatchOptions options;
        options.CustomFinalCondition = [&] { return DecoderFinished; };
        Runtime.DispatchEvents(options, TDuration::Seconds(10));
        UNIT_ASSERT_C(DecoderFinished, "decoder did not handle cancellation");
        UNIT_ASSERT_C(Runtime.FindActor(ReaderId), "reader must remain alive for the late credential continuation");
    }

    void DrainCredentialContinuation() {
        TDispatchOptions options;
        options.CustomFinalCondition = [&] { return CredentialContinuations > 0; };
        Runtime.DispatchEvents(options, TDuration::Seconds(10));
        UNIT_ASSERT_C(CredentialContinuations > 0, "late credential continuation was not delivered");
        Execute([](TFakeActor&) {});
    }

    void ReadToEnd() {
        for (ui32 step = 0; step < 100 && !Result.Finished && Result.Errors.empty(); ++step) {
            Pump();
        }
        UNIT_ASSERT_C(Result.Finished || !Result.Errors.empty(), "HTTP reader did not complete");
    }

private:
    void Execute(TCallback callback) {
        std::exception_ptr exception;
        auto promise = NThreading::NewPromise();
        Runtime.Send(new IEventHandle(FakeActorId, Runtime.AllocateEdgeActor(), new TEvPrivate::TEvExecute(promise, callback, exception)));
        TDispatchOptions options;
        options.CustomFinalCondition = [&] { return promise.HasValue(); };
        Runtime.DispatchEvents(options, TDuration::Seconds(10));
        UNIT_ASSERT_C(promise.HasValue(), "compute actor callback did not complete");
        if (exception) {
            std::rethrow_exception(exception);
        }
    }

    TTestActorRuntimeBase Runtime{1};
    const TActorId FakeActorId{0, "FakeActor"};
    TActorId DecoderId;
    TActorId ReaderId;
    bool DecoderFinished = false;
    ui32 CredentialContinuations = 0;
};

TReadResult ReadFooter(const std::vector<i64>& sizes, ui64 parallelRowGroups = 0, bool selectColumn = true) {
    TParquetReader reader;
    auto gateway = std::make_shared<TParquetGateway>();
    gateway->Body = MakeParquetWithCompressedSizes(sizes);
    reader.Start(gateway, ParquetSource(parallelRowGroups, selectColumn));
    reader.ReadToEnd();
    UNIT_ASSERT(gateway->RangeCalls > 0);
    return reader.Result;
}

void AssertBadRequest(const TReadResult& result) {
    UNIT_ASSERT_C(!result.Errors.empty(), "expected a corrupt-file error");
    for (auto code : result.Errors) {
        UNIT_ASSERT_C(code == NDqProto::StatusIds::BAD_REQUEST, "status=" << static_cast<int>(code) << " " << result.Issues);
    }
}

void CheckReaderCount(ui64 parallelRowGroups, ui64 expectedCount, ui64 dataInflight = 200_MB) {
    TParquetReader reader;
    auto gateway = std::make_shared<TParquetGateway>();
    gateway->Body = MakeParquet(10);
    gateway->HoldChunks = true;
    reader.Start(gateway, ParquetSource(parallelRowGroups), dataInflight);
    reader.Pump();
    UNIT_ASSERT_VALUES_EQUAL_C(gateway->PendingChunks.size(), expectedCount, reader.Result.Issues);
    gateway->ReleaseChunks();
    reader.ReadToEnd();
    UNIT_ASSERT_C(reader.Result.Errors.empty(), reader.Result.Issues);
    UNIT_ASSERT_VALUES_EQUAL(reader.Result.Rows, 10);
    std::sort(reader.Result.Values.begin(), reader.Result.Values.end());
    std::vector<i64> expected;
    for (i64 i = 0; i < 10; ++i) {
        expected.push_back(i * 1000000);
    }
    UNIT_ASSERT_VALUES_EQUAL(reader.Result.Values, expected);
}

} // namespace

Y_UNIT_TEST_SUITE(S3ExternalSecretCancellation) {
    Y_UNIT_TEST(StreamingCancelBeforeHookPublication) {
        NDB::registerFormats();
        auto gateway = std::make_shared<TStreamingGateway>();
        gateway->Body = "value\ninvalid-integer\n";
        auto error = NThreading::NewPromise<TDecoderError>();
        TFakeCASetup setup;
        // Release before setup teardown even when an assertion fails.
        Y_DEFER { gateway->ReleaseHook.TrySetValue(); };
        auto recipient = setup.Runtime->Register(new TDecoderErrorRecipient(error));
        setup.Execute([&](TFakeActor& actor) {
            auto source = CsvSource();
            NS3::TRange range;
            auto* path = range.AddPaths();
            path->SetName("file.csv");
            path->SetSize(gateway->Body.size());
            path->SetRead(true);
            TStringStream encodedRange;
            range.Save(&encodedRange);
            auto [input, inputActor] = CreateS3ReadActor(actor.TypeEnv, actor.HolderFactory,
                std::shared_ptr<TScopedAlloc>(&actor.Alloc, [](TScopedAlloc*) {}), gateway, std::move(source),
                0, TCollectStatsLevel::None, TTxId{}, {}, {}, {encodedRange.Str()}, recipient,
                CreateStructuredTokenCredentialsFactory(), GetHTTPDefaultRetryPolicy(), {}, nullptr, nullptr,
                std::make_shared<TGuaranteeQuotaManager>(200_MB, 200_MB), false);
            actor.InitAsyncInput(input, inputActor);
        });
        UNIT_ASSERT_C(error.GetFuture().Wait(TDuration::Seconds(10)), "decoder did not report a parse error");
        const auto& result = error.GetFuture().GetValue();
        UNIT_ASSERT_C(result.Code == NDqProto::StatusIds::BAD_REQUEST, result.Issues);
        UNIT_ASSERT_STRING_CONTAINS(result.Issues, "invalid-in");
        UNIT_ASSERT_STRING_CONTAINS(result.Issues, "Int32");
        UNIT_ASSERT_VALUES_EQUAL(gateway->CancelCalls.load(), 0);
        gateway->ReleaseHook.TrySetValue();
        // A round trip through the parent's mailbox occurs after Download returned
        // and the production continuation published its hook.
        setup.Execute([](TFakeActor&) {});
        UNIT_ASSERT_C(!gateway->ReleaseTimedOut.load(), "gateway release timed out: invalid race fixture");
        UNIT_ASSERT_VALUES_EQUAL_C(gateway->CancelCalls.load(), 1, "decoder cancellation missed the returned download hook");
    }

    Y_UNIT_TEST(CancelBeforeCredentialsResolve) {
        NDB::registerFormats();
        TParquetReader reader;
        auto gateway = std::make_shared<TStreamingGateway>();
        gateway->Body = "value\n42\n";
        gateway->HoldHook = false;
        auto credentials = std::make_shared<NTests::TAsyncCredentialsFactory>();
        auto source = CsvSource();
        source.SetToken("test-credentials");
        reader.Start(gateway, std::move(source), 200_MB, credentials);
        reader.Pump();
        UNIT_ASSERT_VALUES_EQUAL(credentials->State->Calls, 1);
        UNIT_ASSERT_VALUES_EQUAL(credentials->State->Pending.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(gateway->StreamCalls.load(), 0);
        reader.CancelDecoder();
        credentials->Release("late-token");
        reader.DrainCredentialContinuation();
        UNIT_ASSERT_VALUES_EQUAL(gateway->StreamCalls.load(), 0);
        UNIT_ASSERT_VALUES_EQUAL(gateway->CancelCalls.load(), 0);
        UNIT_ASSERT(reader.Result.Errors.empty());
    }

    Y_UNIT_TEST(StreamingReadStillCompletes) {
        NDB::registerFormats();
        TParquetReader reader;
        auto gateway = std::make_shared<TStreamingGateway>();
        gateway->Body = "value\n42\n";
        gateway->HoldHook = false;
        reader.Start(gateway, CsvSource());
        reader.ReadToEnd();
        UNIT_ASSERT_C(reader.Result.Errors.empty(), reader.Result.Issues);
        UNIT_ASSERT(reader.Result.Finished);
        UNIT_ASSERT_VALUES_EQUAL(reader.Result.DeliveredRows, 1);
        UNIT_ASSERT_VALUES_EQUAL(gateway->StreamCalls.load(), 1);
    }
}

Y_UNIT_TEST_SUITE(TS3ParquetFooter) {

    // New range requests in one real reader must reacquire credentials. Held
    // HTTP responses/retries are a different lifetime and are not relabeled.
    Y_UNIT_TEST(LaterParquetRangesUseNewCredentialsWithoutRestartingReader) {
        TParquetReader reader;
        auto gateway = std::make_shared<TParquetGateway>();
        gateway->Body = MakeParquet(3);
        auto credentials = std::make_shared<NTests::TAsyncCredentialsFactory>();
        gateway->OnRange = [credentials] { credentials->State->Held = true; };
        auto source = ParquetSource();
        source.SetToken("test-credentials");
        reader.Start(gateway, std::move(source), 200_MB, credentials);
        reader.Pump();
        for (ui32 round = 0; round < 20 && !reader.Result.Finished && reader.Result.Errors.empty(); ++round) {
            UNIT_ASSERT(!credentials->State->Pending.empty());
            const auto before = gateway->Tokens.size();
            const TString token = round == 0 ? "original-token"
                : round == 1 ? "altered-binding-token" : "refreshed-token";
            credentials->Release(std::string(token));
            reader.Pump();
            UNIT_ASSERT(gateway->Tokens.size() > before);
            for (size_t index = before; index < gateway->Tokens.size(); ++index) {
                UNIT_ASSERT_VALUES_EQUAL(gateway->Tokens[index], token);
            }
        }
        UNIT_ASSERT_C(reader.Result.Errors.empty(), reader.Result.Issues);
        UNIT_ASSERT(reader.Result.Finished);
        UNIT_ASSERT_VALUES_EQUAL(reader.Result.Rows, 3);
        UNIT_ASSERT(std::find(gateway->Tokens.begin(), gateway->Tokens.end(), "altered-binding-token") != gateway->Tokens.end());
        UNIT_ASSERT(std::find(gateway->Tokens.begin(), gateway->Tokens.end(), "refreshed-token") != gateway->Tokens.end());
        UNIT_ASSERT_VALUES_EQUAL(credentials->State->Calls, gateway->RangeCalls);
    }

    Y_UNIT_TEST(LaterParquetAuthorizationDenialNeverReusesEarlierToken) {
        TParquetReader reader;
        auto gateway = std::make_shared<TParquetGateway>();
        gateway->Body = MakeParquet(3);
        auto credentials = std::make_shared<NTests::TAsyncCredentialsFactory>();
        gateway->OnRange = [credentials] { credentials->State->Held = true; };
        auto source = ParquetSource();
        source.SetToken("test-credentials");
        reader.Start(gateway, std::move(source), 200_MB, credentials);
        reader.Pump();
        credentials->Release("first-range-token");
        reader.Pump();
        UNIT_ASSERT(gateway->RangeCalls > 0);
        UNIT_ASSERT(!reader.Result.Finished);
        UNIT_ASSERT(!credentials->State->Pending.empty());
        const auto authorizedRanges = gateway->RangeCalls;
        credentials->Fail();
        reader.ReadToEnd();
        UNIT_ASSERT(!reader.Result.Errors.empty());
        UNIT_ASSERT_VALUES_EQUAL(gateway->RangeCalls, authorizedRanges);
        for (const auto& token : gateway->Tokens) {
            UNIT_ASSERT_VALUES_EQUAL(token, "first-range-token");
        }
    }

    Y_UNIT_TEST(ParquetRangesAwaitCredentialsWithoutBlockingActor) {
        TParquetReader reader;
        auto gateway = std::make_shared<TParquetGateway>();
        gateway->Body = MakeParquet(3);
        auto credentials = std::make_shared<NTests::TAsyncCredentialsFactory>();
        reader.Start(gateway, ParquetSource(), 200_MB, credentials);
        reader.Pump();
        UNIT_ASSERT(credentials->State->Calls > 0);
        UNIT_ASSERT_VALUES_EQUAL(gateway->RangeCalls, 0);
        credentials->Release("fresh-token");
        reader.ReadToEnd();
        UNIT_ASSERT_C(reader.Result.Errors.empty(), reader.Result.Issues);
        UNIT_ASSERT_VALUES_EQUAL(reader.Result.Rows, 3);
        UNIT_ASSERT_VALUES_EQUAL(credentials->State->Calls, gateway->RangeCalls);
    }

    Y_UNIT_TEST(ParquetAuthorizationFailureNeverStartsHttpRequest) {
        TParquetReader reader;
        auto gateway = std::make_shared<TParquetGateway>();
        gateway->Body = MakeParquet(1);
        auto credentials = std::make_shared<NTests::TAsyncCredentialsFactory>();
        reader.Start(gateway, ParquetSource(), 200_MB, credentials);
        reader.Pump();
        credentials->Fail();
        reader.ReadToEnd();
        UNIT_ASSERT(!reader.Result.Errors.empty());
        UNIT_ASSERT_VALUES_EQUAL(gateway->RangeCalls, 0);
    }

    Y_UNIT_TEST(ZeroCompressedSize) {
        for (ui64 parallel : {0, 1, 2}) {
            auto result = ReadFooter({0}, parallel);
            UNIT_ASSERT_C(result.Errors.empty(), result.Issues);
            UNIT_ASSERT(result.Finished);
            UNIT_ASSERT_VALUES_EQUAL(result.Rows, 0);
        }
    }

    Y_UNIT_TEST(NegativeCompressedSize) {
        for (ui64 parallel : {0, 1, 2}) {
            AssertBadRequest(ReadFooter({-1}, parallel));
        }
    }

    Y_UNIT_TEST(DoubledCompressedSizeOverflow) {
        for (ui64 parallel : {0, 1, 2}) {
            AssertBadRequest(ReadFooter({i64(1) << 62, i64(1) << 62}, parallel));
        }
    }

    Y_UNIT_TEST(CompressedSizeSumOverflow) {
        for (ui64 parallel : {0, 1, 2}) {
            const auto result = ReadFooter({Max<i64>() - 4, Max<i64>() - 4, Max<i64>() - 4}, parallel);
            AssertBadRequest(result);
            UNIT_ASSERT_STRING_CONTAINS(result.Issues, "total_compressed_size sum overflow");
        }
    }

    Y_UNIT_TEST(LargeCompressedSize) {
        for (ui64 parallel : {0, 1, 2}) {
            AssertBadRequest(ReadFooter({Max<i64>() - 4}, parallel));
        }
    }

    Y_UNIT_TEST(EmptyFile) {
        for (ui64 parallel : {0, 1, 2}) {
            auto result = ReadFooter({}, parallel);
            UNIT_ASSERT_C(result.Errors.empty(), result.Issues);
            UNIT_ASSERT(result.Finished);
            UNIT_ASSERT_VALUES_EQUAL(result.Rows, 0);
        }
    }

    Y_UNIT_TEST(NoSelectedColumns) {
        for (ui64 parallel : {0, 1, 2}) {
            TParquetReader reader;
            auto gateway = std::make_shared<TParquetGateway>();
            gateway->Body = MakeParquet(10);
            reader.Start(gateway, ParquetSource(parallel, false));
            reader.ReadToEnd();
            UNIT_ASSERT_C(reader.Result.Errors.empty(), reader.Result.Issues);
            UNIT_ASSERT_VALUES_EQUAL(reader.Result.Rows, 10);
            UNIT_ASSERT(reader.Result.Values.empty());
        }
    }

    Y_UNIT_TEST(AutomaticReaderCount) {
        CheckReaderCount(0, 5);
        CheckReaderCount(0, 1, 1);
    }

    Y_UNIT_TEST(ExplicitReaderCount) {
        for (ui64 parallel : {1, 2, 5, 20}) {
            CheckReaderCount(parallel, std::min(parallel, ui64(10)));
        }
    }

    Y_UNIT_TEST(FairShareMultiplicationOverflow) {
        CheckReaderCount(0, 5, ui64(1) << 63);
    }
}

} // namespace NYql::NDq
