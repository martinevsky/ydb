// Transport tests for the S3 read actor (target G of the FQ transport test plan, test_plan.md §4):
// the streaming read coroutine (CSV) and the ranged reads (parquet) against H9 TScriptedHttpGateway,
// which completes every download on the test's schedule (no network).
//
// The read actor runs under TFakeCASetup (real threads, because the coroutine and the gateway callbacks
// run on actor-system threads). Waits are guarded sync points: the fake compute actor's data and error
// futures, H9 calls and cancels (§6.3 D-1). The only windows are the "no retry after cancel" checks of
// T-LIF-10: nothing is observable when a retry correctly does not happen, so they wait 2.5x the retry
// delay (D-3) after a positive sync point. Contract tests that fail today start with YDB_SKIP_KNOWN_BUG
// (run them with --test-env=YDB_TRANSPORT_RUN_KNOWN_BUGS=1).

#include <ydb/library/yql/providers/s3/actors/yql_s3_actors_util.h>
#include <ydb/library/yql/providers/s3/actors/yql_s3_read_actor.h>
#include <ydb/library/yql/providers/s3/proto/range.pb.h>

#include <ydb/library/yql/dq/actors/compute/dq_compute_actor.h>
#include <ydb/library/yql/providers/common/http_gateway/mock/yql_http_scripted_gateway.h>
#include <ydb/library/yql/providers/common/http_gateway/yql_http_default_retry_policy.h>
#include <ydb/library/yql/providers/common/ut_helpers/dq_fake_ca.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/known_bug.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/wait.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/Formats/registerFormats.h>

#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/threading/future/wait/wait.h>

#include <util/generic/size_literals.h>
#include <util/string/builder.h>

#include <mutex>

namespace NYql::NDq {

namespace {

using namespace NKikimr::NMiniKQL;
using namespace NYql::NTransportTest;

using EStatus = NYql::NDqProto::StatusIds::StatusCode;
using TFiles = TVector<std::pair<TString, ui64>>;

const TString URL = "http://fake/";
const TString CSV_ROW_TYPE = R"(["StructType";[["a";["DataType";"String"]]]])";
const TString PARQUET_ROW_TYPE = R"(["StructType";[["ts";["DataType";"Timestamp"]]]])";
const TString SLOW_DOWN_XML = "<?xml version=\"1.0\"?><Error><Code>SlowDown</Code><Message>Please reduce your request rate.</Message></Error>";
const TString ACCESS_DENIED_XML = "<?xml version=\"1.0\"?><Error><Code>AccessDenied</Code><Message>Access Denied</Message></Error>";

void RegisterClickHouseFormats() {
    static std::once_flag once;
    std::call_once(once, []() {
        NDB::registerFormats();
    });
}

TString EncodeRange(const TFiles& files) {
    NS3::TRange range;
    for (const auto& [name, size] : files) {
        auto* path = range.AddPaths();
        path->SetName(name);
        path->SetSize(size);
        path->SetRead(true);
    }
    TStringStream out;
    range.Save(&out);
    return out.Str();
}

NS3::TSource CsvSource() {
    NS3::TSource source;
    source.SetUrl(URL);
    source.SetFormat("csv_with_names");
    source.SetRowType(CSV_ROW_TYPE);
    return source;
}

NS3::TSource ParquetSource() {
    NS3::TSource source;
    source.SetUrl(URL);
    source.SetFormat("parquet");
    source.SetRowType(PARQUET_ROW_TYPE);
    return source;
}

// A CSV object with the header "a" and `rows` rows "x" (2 bytes per line).
TString CsvBody(ui64 rows) {
    TStringBuilder body;
    body << "a\n";
    for (ui64 i = 0; i < rows; ++i) {
        body << "x\n";
    }
    return body;
}

// Retries every error after about `delay` (randomized to [delay/2, delay]).
IHTTPGateway::TRetryPolicy::TPtr FixedDelayRetryPolicy(TDuration delay) {
    return IHTTPGateway::TRetryPolicy::GetFixedIntervalPolicy(
        [](CURLcode, long) {
            return ERetryErrorClass::ShortRetry;
        },
        delay,
        delay,
        10);
}

TString Head(const TString& text) {
    return text.size() <= 300 ? text : text.substr(0, 300) + "...";
}

size_t StreamCalls(const TScriptedHttpGateway& gateway, TStringBuf urlSuffix = {}) {
    size_t count = 0;
    for (const auto& call : gateway.Calls()) {
        if (call.Method == TScriptedHttpGateway::EMethod::GetStream && call.Url.EndsWith(urlSuffix)) {
            ++count;
        }
    }
    return count;
}

// True when `predicate` holds within `window`. Only for "must not happen" checks (§6.3 D-3).
bool HappensWithin(const std::function<bool()>& predicate, TDuration window) {
    try {
        WaitUntil(predicate, window, "an event inside a negative window");
        return true;
    } catch (const yexception&) {
        return false;
    }
}

struct TReadResult {
    ui64 Rows = 0;
    bool Finished = false;
    std::vector<TAsyncInputError> Errors;

    const TAsyncInputError* Fatal() const {
        for (const auto& error : Errors) {
            if (error.FatalCode != NYql::NDqProto::StatusIds::UNSPECIFIED) {
                return &error;
            }
        }
        return nullptr;
    }

    TString Describe() const {
        TStringBuilder text;
        text << "rows=" << Rows << " finished=" << Finished << " errors=[";
        for (const auto& error : Errors) {
            text << NYql::NDqProto::StatusIds::StatusCode_Name(error.FatalCode) << ": " << Head(error.Issues.ToOneLineString()) << "; ";
        }
        return text << "]";
    }
};

// One S3 read actor under a fake compute actor, reading `files` through `gateway`.
class TS3ReadRun {
public:
    TS3ReadRun(TScriptedHttpGateway::TPtr gateway, NS3::TSource source, const TFiles& files,
        IHTTPGateway::TRetryPolicy::TPtr retryPolicy = GetHTTPDefaultRetryPolicy())
    {
        RegisterClickHouseFormats();
        Setup.Execute([&](TFakeActor& actor) {
            auto [input, asActor] = CreateS3ReadActor(actor.TypeEnv, actor.HolderFactory,
                std::shared_ptr<TScopedAlloc>(&actor.Alloc, [](TScopedAlloc*) {}),
                gateway, std::move(source), 0, TCollectStatsLevel::None, TTxId{},
                THashMap<TString, TString>{}, THashMap<TString, TString>{},
                TVector<TString>{EncodeRange(files)}, Setup.FakeActorId, CreateStructuredTokenCredentialsFactory(),
                retryPolicy, TS3ReadActorFactoryConfig{}, nullptr, nullptr,
                std::make_shared<TGuaranteeQuotaManager>(1_GB, 1_GB), false);
            actor.InitAsyncInput(input, asActor);
        });
    }

    ~TS3ReadRun() {
        Setup.Terminate();
    }

    // Pulls data until the input reports finished (or, with stopOnFatal, a fatal error arrives).
    // Between pulls it waits for the next data or error notification of the fake compute actor.
    TReadResult ReadToEnd(TDuration guard = TDuration::Seconds(20), bool stopOnFatal = true) {
        const TInstant deadline = guard.ToDeadLine();
        while (true) {
            NThreading::TFuture<void> dataArrived;
            NThreading::TFuture<TIssues> errorArrived;
            Setup.Execute([&](TFakeActor& actor) {
                TMaybe<TInstant> watermark;
                TUnboxedValueBatch buffer;
                bool finished = false;
                actor.DqAsyncInput->GetAsyncInputData(buffer, watermark, finished, 1_MB);
                Result.Rows += buffer.RowCount();
                Result.Finished = finished;
                Result.Errors = Setup.AsyncInputPromises->Errors;
                dataArrived = Setup.AsyncInputPromises->NewAsyncInputDataArrived.GetFuture();
                errorArrived = Setup.AsyncInputPromises->FatalError.GetFuture();
            });
            if (Result.Finished || (stopOnFatal && Result.Fatal())) {
                return Result;
            }
            const bool notified = NThreading::WaitAny(dataArrived, errorArrived.IgnoreResult()).Wait(deadline);
            Y_ENSURE(notified, "guard of " << guard << " expired reading the S3 object: " << Result.Describe());
        }
    }

    // Waits until the fake compute actor has received `count` errors (retriable or fatal).
    void WaitForErrors(size_t count, TDuration guard = TDuration::Seconds(10)) {
        const TInstant deadline = guard.ToDeadLine();
        while (true) {
            size_t errors = 0;
            NThreading::TFuture<TIssues> errorArrived;
            Setup.Execute([&](TFakeActor&) {
                errors = Setup.AsyncInputPromises->Errors.size();
                errorArrived = Setup.AsyncInputPromises->FatalError.GetFuture();
            });
            if (errors >= count) {
                return;
            }
            Y_ENSURE(errorArrived.Wait(deadline), "guard of " << guard << " expired waiting for " << count << " async input error(s)");
        }
    }

    // Passes the read actor away, as the compute actor does when the task ends.
    void Terminate() {
        Setup.Terminate();
    }

    // The errors the fake compute actor has received so far.
    std::vector<TAsyncInputError> Errors() {
        std::vector<TAsyncInputError> errors;
        Setup.Execute([&](TFakeActor&) {
            errors = Setup.AsyncInputPromises->Errors;
        });
        return errors;
    }

private:
    TFakeCASetup Setup;
    TReadResult Result;
};

// LIMIT reached while a coroutine waits for its retry (s3fq #20). Two files in one file-queue batch,
// so both coroutines run at once: b.csv fails and schedules a retry after `Delay` (randomized to
// [Delay/2, Delay]); then a.csv delivers `Limit` rows, the stream actor reaches the rows limit and
// poisons every coroutine, including b.csv's, which is in its backoff. The stream actor stays alive
// (the fake compute actor does not pass it away), as in a query whose LIMIT is satisfied.
class TLimitDuringBackoff {
public:
    static constexpr TDuration Delay = TDuration::Seconds(2);
    static constexpr ui64 Limit = 1001; // a hint <= 1000 would force ParallelDownloadCount = 1

    TLimitDuringBackoff()
        : Gateway(TScriptedHttpGateway::Make())
        , BodyA(CsvBody(Limit))
        , BodyB(CsvBody(1))
    {
        Gateway->SetStreamScript([this](const TScriptedHttpGateway::TStreamPtr& stream) {
            if (stream->Call().Url.EndsWith("a.csv")) {
                std::lock_guard lock(Mutex);
                StreamA = stream; // driven by Run()
                return;
            }
            stream->Start(CURLE_OK, 200);
            if (stream->Call().Attempt == 0) {
                stream->Finish(CURLE_RECV_ERROR, {TIssue("scripted receive failure")});
            } else {
                stream->Data(BodyB.substr(stream->Call().Offset));
                stream->Finish(CURLE_OK);
            }
        });

        auto source = CsvSource();
        source.SetRowsLimitHint(Limit);
        // One batch with both objects (by default a batch holds one object and the next batch starts
        // only after the previous coroutine has ended).
        (*source.MutableSettings())["fileQueueBatchObjectCountLimit"] = "10";
        (*source.MutableSettings())["fileQueueBatchSizeLimit"] = ToString(1_MB);
        ReadRun = std::make_unique<TS3ReadRun>(Gateway, std::move(source),
            TFiles{{"a.csv", BodyA.size()}, {"b.csv", BodyB.size()}}, FixedDelayRetryPolicy(Delay));
    }

    // Drives the scenario up to the LIMIT poison; returns once the stream actor reports finished.
    TReadResult Run() {
        Gateway->WaitForCalls(2);
        ReadRun->WaitForErrors(1); // b.csv failed; its retry is scheduled in the same handler

        TScriptedHttpGateway::TStreamPtr streamA;
        {
            std::lock_guard lock(Mutex);
            streamA = StreamA;
        }
        Y_ENSURE(streamA, "a.csv was not requested");
        streamA->Start(CURLE_OK, 200);
        streamA->Data(BodyA);
        streamA->Finish(CURLE_OK);

        return ReadRun->ReadToEnd(TDuration::Seconds(20), /*stopOnFatal=*/false);
    }

    // Declaration order matters: ReadRun is destroyed first, so no gateway callback can run into the
    // script's state after it is gone.
    const TScriptedHttpGateway::TPtr Gateway;
    const TString BodyA;
    const TString BodyB;
    std::mutex Mutex;
    TScriptedHttpGateway::TStreamPtr StreamA;
    std::unique_ptr<TS3ReadRun> ReadRun;
};

} // namespace

Y_UNIT_TEST_SUITE(TS3ReadActorTransportTest) {

    // T-LIF-10 case 1 (pin): the read actor dies while a stream is in flight; its cancel hook runs once.
    Y_UNIT_TEST(StreamCancelledOnReadActorDeath) {
        auto gateway = TScriptedHttpGateway::Make();
        gateway->SetStreamScript([](const TScriptedHttpGateway::TStreamPtr& stream) {
            stream->Start(CURLE_OK, 200);
            stream->Data("a\nx\n");
            // then holds: no Finish
        });

        TS3ReadRun run(gateway, CsvSource(), {{"file", 1_MB}});
        gateway->WaitForCalls(1);
        run.Terminate();

        WaitUntil([&]() {
            return !gateway->Cancels().empty();
        }, TDuration::Seconds(10), "the stream's cancel hook runs");
        UNIT_ASSERT_VALUES_EQUAL(gateway->Cancels().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(gateway->Streams()[0]->FinishCalls(), 1);
        UNIT_ASSERT_VALUES_EQUAL(StreamCalls(*gateway), 1);
    }

    // T-LIF-10 case 2, as the plan words it: the whole read actor dies while a retry is scheduled.
    // The retry event is addressed to the (dead) stream actor, so no new download starts.
    Y_UNIT_TEST(NoRetryAfterReadActorDeathDuringBackoff) {
        const TDuration delay = TDuration::Seconds(1);
        const TString body = CsvBody(1);
        auto gateway = TScriptedHttpGateway::Make();
        gateway->SetStreamScript([body](const TScriptedHttpGateway::TStreamPtr& stream) {
            stream->Start(CURLE_OK, 200);
            if (stream->Call().Attempt == 0) {
                stream->Finish(CURLE_RECV_ERROR, {TIssue("scripted receive failure")});
            } else {
                stream->Data(body.substr(stream->Call().Offset));
                stream->Finish(CURLE_OK);
            }
        });

        TS3ReadRun run(gateway, CsvSource(), {{"file", body.size()}}, FixedDelayRetryPolicy(delay));
        run.WaitForErrors(1); // the retriable error: the retry is scheduled in the same handler
        run.Terminate();

        UNIT_ASSERT_C(!HappensWithin([&]() { return StreamCalls(*gateway) >= 2; }, delay * 5 / 2),
            "a download started after the read actor was passed away");
    }

    // T-LIF-10 case 2 (s3fq #20): the coroutine is cancelled during its backoff while the stream actor
    // lives on (the LIMIT path, see TLimitDuringBackoff). The scheduled retry reaches the live stream
    // actor, whose HandleRetry calls DownloadStart unconditionally: b.csv is downloaded again.
    Y_UNIT_TEST(NoRetryAfterLimitPoisonDuringBackoff) {
        YDB_SKIP_KNOWN_BUG("s3fq#20");

        TLimitDuringBackoff scenario;
        const auto result = scenario.Run();
        UNIT_ASSERT_C(result.Finished, result.Describe());
        UNIT_ASSERT_VALUES_EQUAL(StreamCalls(*scenario.Gateway, "b.csv"), 1);

        UNIT_ASSERT_C(!HappensWithin([&]() { return StreamCalls(*scenario.Gateway, "b.csv") >= 2; }, TLimitDuringBackoff::Delay * 5 / 2),
            "b.csv was downloaded again after the LIMIT poison cancelled its coroutine");
    }

    // Candidate finding N-6 (found while writing T-LIF-10): the coroutine poisoned during its backoff
    // aborts with the previous attempt's retriable issues still in Issues, and Run() reports them as a
    // fatal EXTERNAL_ERROR, although the read finished because the LIMIT was reached.
    Y_UNIT_TEST(LimitPoisonDuringBackoffIsNotAnError) {
        YDB_SKIP_KNOWN_BUG("N-6");

        TLimitDuringBackoff scenario;
        const auto result = scenario.Run();
        UNIT_ASSERT_C(result.Finished, result.Describe());

        std::vector<TAsyncInputError> errors;
        const auto fatalArrived = [&]() {
            errors = scenario.ReadRun->Errors();
            for (const auto& error : errors) {
                if (error.FatalCode != NYql::NDqProto::StatusIds::UNSPECIFIED) {
                    return true;
                }
            }
            return false;
        };
        const bool fatal = HappensWithin(fatalArrived, TDuration::Seconds(1));
        TReadResult reported;
        reported.Finished = true;
        reported.Errors = errors;
        UNIT_ASSERT_C(!fatal, "a fatal error was reported after the rows limit was reached: " << reported.Describe());
    }

    // T-RTY-8 (F-A-3): every attempt makes progress (one 2-byte part) and then fails; the retry budget
    // (MaxRetries = 3) must restart after progress, so 6 failures in a row still complete the read.
    Y_UNIT_TEST(RetryBudgetResetsAfterProgress) {
        YDB_SKIP_KNOWN_BUG("F-A-3");

        constexpr ui64 rows = 6;
        const TString body = CsvBody(rows);
        auto gateway = TScriptedHttpGateway::Make();
        gateway->SetStreamScript([body](const TScriptedHttpGateway::TStreamPtr& stream) {
            const auto& call = stream->Call();
            stream->Start(CURLE_OK, call.Offset ? 206 : 200);
            if (call.Attempt < rows) {
                stream->Data(body.substr(call.Offset, 2));
                stream->Finish(CURLE_RECV_ERROR, {TIssue("scripted receive failure")});
            } else {
                stream->Data(body.substr(call.Offset));
                stream->Finish(CURLE_OK);
            }
        });

        THttpRetryPolicyOptions options;
        options.MaxRetries = 3;
        TS3ReadRun run(gateway, CsvSource(), {{"file", body.size()}}, GetHTTPDefaultRetryPolicy(std::move(options)));
        const auto result = run.ReadToEnd();

        UNIT_ASSERT_C(result.Finished && !result.Fatal(), result.Describe() << " stream calls: " << StreamCalls(*gateway));
        UNIT_ASSERT_VALUES_EQUAL(result.Rows, rows);
        UNIT_ASSERT_VALUES_EQUAL(StreamCalls(*gateway), rows + 1);
    }

    // T-RTY-9 (s3fq #3): a streaming retry after a 503 with an S3 XML body succeeds.
    Y_UNIT_TEST(StreamRetryAfter503WithBodySucceeds) {
        YDB_SKIP_KNOWN_BUG("s3fq#3");

        const TString body = CsvBody(1);
        auto gateway = TScriptedHttpGateway::Make();
        gateway->SetStreamScript([body](const TScriptedHttpGateway::TStreamPtr& stream) {
            if (stream->Call().Attempt == 0) {
                stream->Start(CURLE_OK, 503);
                stream->Data(SLOW_DOWN_XML);
                stream->Finish(CURLE_OK);
            } else {
                stream->Start(CURLE_OK, 206);
                stream->Data(body);
                stream->Finish(CURLE_OK);
            }
        });

        TS3ReadRun run(gateway, CsvSource(), {{"file", body.size()}});
        const auto result = run.ReadToEnd();

        UNIT_ASSERT_VALUES_EQUAL(StreamCalls(*gateway), 2);
        UNIT_ASSERT_C(result.Finished && !result.Fatal(), result.Describe());
        UNIT_ASSERT_VALUES_EQUAL(result.Rows, 1);
    }

    // T-ERR-7 (s3fq #21), parquet: a ranged GET answered 403 AccessDenied is a user/storage error,
    // mapped from the S3 code, not INTERNAL_ERROR.
    Y_UNIT_TEST(ParquetRangedGet403IsNotInternalError) {
        YDB_SKIP_KNOWN_BUG("s3fq#21");

        auto gateway = TScriptedHttpGateway::Make();
        gateway->SetBufferedScript([](const TScriptedHttpGateway::TCall&) -> std::optional<IHTTPGateway::TResult> {
            return IHTTPGateway::TResult(IHTTPGateway::TContent(ACCESS_DENIED_XML, 403));
        });

        TS3ReadRun run(gateway, ParquetSource(), {{"file.parquet", 1_KB}});
        const auto result = run.ReadToEnd();

        const auto* fatal = result.Fatal();
        UNIT_ASSERT_C(fatal, result.Describe());
        UNIT_ASSERT_VALUES_EQUAL_C(NYql::NDqProto::StatusIds::StatusCode_Name(fatal->FatalCode),
            NYql::NDqProto::StatusIds::StatusCode_Name(StatusFromS3ErrorCode("AccessDenied")), result.Describe());
    }

    // T-ERR-7 (s3fq #21), streaming: a 404 with a non-XML body is EXTERNAL_ERROR, not INTERNAL_ERROR.
    Y_UNIT_TEST(Stream404NonXmlBodyIsExternalError) {
        YDB_SKIP_KNOWN_BUG("s3fq#21");

        auto gateway = TScriptedHttpGateway::Make();
        gateway->SetStreamScript([](const TScriptedHttpGateway::TStreamPtr& stream) {
            stream->Start(CURLE_OK, 404);
            stream->Data("Not Found");
            stream->Finish(CURLE_OK);
        });

        TS3ReadRun run(gateway, CsvSource(), {{"file", 1_KB}});
        const auto result = run.ReadToEnd();

        const auto* fatal = result.Fatal();
        UNIT_ASSERT_C(fatal, result.Describe());
        UNIT_ASSERT_VALUES_EQUAL_C(NYql::NDqProto::StatusIds::StatusCode_Name(fatal->FatalCode),
            NYql::NDqProto::StatusIds::StatusCode_Name(NYql::NDqProto::StatusIds::EXTERNAL_ERROR), result.Describe());
    }
}

} // namespace NYql::NDq
