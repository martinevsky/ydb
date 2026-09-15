#include <ydb/library/yql/providers/s3/actors/yql_s3_source_queue.h>
#include <ydb/library/yql/providers/s3/events/events.h>
#include <ydb/library/yql/dq/actors/common/retry_queue.h>

#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/core/interconnect.h>
#include <ydb/library/actors/interconnect/interconnect.h>
#include <ydb/library/actors/interconnect/interconnect_impl.h>
#include <ydb/library/actors/testlib/test_runtime.h>
#include <ydb/library/services/services.pb.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/size_literals.h>
#include <util/string/join.h>

using namespace NActors;
using namespace NYql::NDq;

namespace {

// Test events exchanged between the test body (edge actor) and the consumer double.
struct TEvTest {
    enum EEv : ui32 {
        EvBegin = TEvRetryQueuePrivate::EvEnd, // leave space for TRetryEventsQueue private events
        EvRequestBatch = EvBegin,
        EvBatchAccepted,
        EvDuplicateDropped,
        EvEnd
    };
    static_assert(EvEnd < EventSpaceEnd(TEvents::ES_PRIVATE), "expect EvEnd < EventSpaceEnd(TEvents::ES_PRIVATE)");

    // Ask the consumer to send one TEvGetNextBatch through its TRetryEventsQueue.
    struct TEvRequestBatch : public TEventLocal<TEvRequestBatch, EvRequestBatch> {};

    // A batch that passed TRetryEventsQueue::OnEventReceived (== what the real read actor would process).
    struct TEvBatchAccepted : public TEventLocal<TEvBatchAccepted, EvBatchAccepted> {
        ui64 SeqNo = 0;
        bool NoMoreFiles = false;
        std::vector<TString> Paths;
    };

    // A batch that TRetryEventsQueue::OnEventReceived rejected as a duplicate (== silently dropped by the read actor).
    struct TEvDuplicateDropped : public TEventLocal<TEvDuplicateDropped, EvDuplicateDropped> {
        ui64 SeqNo = 0;
        std::vector<TString> Paths;
    };
};

std::vector<TString> ExtractPaths(const NYql::NS3::FileQueue::TEvObjectPathBatch& record) {
    std::vector<TString> paths;
    for (const auto& object : record.GetObjectPaths()) {
        paths.push_back(object.GetPath());
    }
    return paths;
}

// Minimal double of the FileQueue client side of TS3StreamReadActor: the same TRetryEventsQueue wiring
// (Init/OnNewRecipientId/Send/OnEventReceived/Retry/HandleNode*/HandleUndelivered), nothing else.
class TFileQueueConsumer : public TActorBootstrapped<TFileQueueConsumer> {
public:
    TFileQueueConsumer(const TActorId& fileQueue, const TActorId& edge)
        : FileQueue(fileQueue)
        , Edge(edge)
    {}

    void Bootstrap() {
        Become(&TFileQueueConsumer::StateFunc);
        FileQueueEvents.Init(TTxId(TString("test-tx")), SelfId(), SelfId());
        FileQueueEvents.OnNewRecipientId(FileQueue);
    }

    STRICT_STFUNC(StateFunc,
        cFunc(TEvTest::EvRequestBatch, HandleRequestBatch);
        hFunc(TEvS3Provider::TEvObjectPathBatch, HandleObjectPathBatch);
        hFunc(TEvS3Provider::TEvObjectPathReadError, HandleObjectPathReadError);
        hFunc(TEvS3Provider::TEvAck, HandleAck);
        hFunc(TEvRetryQueuePrivate::TEvRetry, HandleRetry);
        hFunc(TEvInterconnect::TEvNodeDisconnected, HandleNodeDisconnected);
        hFunc(TEvInterconnect::TEvNodeConnected, HandleNodeConnected);
        hFunc(TEvents::TEvUndelivered, HandleUndelivered);
    )

private:
    void HandleRequestBatch() {
        FileQueueEvents.Send(new TEvS3Provider::TEvGetNextBatch());
    }

    void HandleObjectPathBatch(TEvS3Provider::TEvObjectPathBatch::TPtr& ev) {
        const auto& record = ev->Get()->Record;
        if (!FileQueueEvents.OnEventReceived(ev)) {
            auto dropped = MakeHolder<TEvTest::TEvDuplicateDropped>();
            dropped->SeqNo = record.GetTransportMeta().GetSeqNo();
            dropped->Paths = ExtractPaths(record);
            Send(Edge, dropped.Release());
            return;
        }
        auto accepted = MakeHolder<TEvTest::TEvBatchAccepted>();
        accepted->SeqNo = record.GetTransportMeta().GetSeqNo();
        accepted->NoMoreFiles = record.GetNoMoreFiles();
        accepted->Paths = ExtractPaths(record);
        Send(Edge, accepted.Release());
    }

    void HandleObjectPathReadError(TEvS3Provider::TEvObjectPathReadError::TPtr& ev) {
        FileQueueEvents.OnEventReceived(ev);
        UNIT_FAIL("unexpected TEvObjectPathReadError");
    }

    void HandleAck(TEvS3Provider::TEvAck::TPtr& ev) {
        FileQueueEvents.OnEventReceived(ev);
    }

    void HandleRetry(const TEvRetryQueuePrivate::TEvRetry::TPtr&) {
        FileQueueEvents.Retry();
    }

    void HandleNodeDisconnected(TEvInterconnect::TEvNodeDisconnected::TPtr& ev) {
        FileQueueEvents.HandleNodeDisconnected(ev->Get()->NodeId);
    }

    void HandleNodeConnected(TEvInterconnect::TEvNodeConnected::TPtr& ev) {
        FileQueueEvents.HandleNodeConnected(ev->Get()->NodeId);
    }

    void HandleUndelivered(TEvents::TEvUndelivered::TPtr& ev) {
        UNIT_ASSERT_C(FileQueueEvents.HandleUndelivered(ev) == TRetryEventsQueue::ESessionState::WrongSession,
            "FileQueue was lost: request was undelivered, the real read actor would fail the query here");
    }

private:
    const TActorId FileQueue;
    const TActorId Edge;
    TRetryEventsQueue FileQueueEvents;
};

IActor* MakeFileQueue(size_t fileCount, ui64 batchObjectCountLimit) {
    NYql::NS3Details::TPathList paths;
    for (size_t i = 0; i < fileCount; ++i) {
        paths.emplace_back(TStringBuilder() << "file-" << i, 100, false, 0);
    }
    return CreateS3FileQueueActor(
        TTxId(TString("test-tx")),
        std::move(paths),
        /* prefetchSize */ 100,
        /* fileSizeLimit */ 1_GB,
        /* readLimit */ 1_GB,
        /* useRuntimeListing */ true,
        /* consumersCount */ 1,
        /* batchSizeLimit */ 1_GB,
        batchObjectCountLimit,
        /* gateway */ nullptr,
        /* retryPolicy */ nullptr,
        /* url */ "",
        NYql::TS3Credentials{},
        /* pattern */ "",
        NYql::NS3Lister::ES3PatternVariant::FilePattern,
        NYql::NS3Lister::ES3PatternType::Wildcard,
        /* allowLocalFiles */ false);
}

std::set<TString> AllFiles(size_t fileCount) {
    std::set<TString> files;
    for (size_t i = 0; i < fileCount; ++i) {
        files.insert(TStringBuilder() << "file-" << i);
    }
    return files;
}

// TS3FileQueueActor logs into NKikimrServices::KQP_COMPUTE, which the bare actor runtime does not know about.
// Must run after Initialize(): the bare runtime creates its per-node log settings there.
void RegisterKikimrLogComponents(TTestActorRuntimeBase& runtime, ui32 nodeCount) {
    for (ui32 nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
        runtime.GetLogSettings(nodeIndex)->Append(
            NKikimrServices::EServiceKikimr_MIN,
            NKikimrServices::EServiceKikimr_MAX,
            NKikimrServices::EServiceKikimr_Name<NActors::NLog::EComponent>);
    }
}

TEvS3Provider::TEvGetNextBatch* MakeGetNextBatch(ui64 seqNo, ui64 confirmedSeqNo) {
    auto* ev = new TEvS3Provider::TEvGetNextBatch();
    ev->Record.MutableTransportMeta()->SetSeqNo(seqNo);
    ev->Record.MutableTransportMeta()->SetConfirmedSeqNo(confirmedSeqNo);
    return ev;
}

struct TTwoNodeRuntime {
    TTestActorRuntimeBase Runtime;
    TActorId Edge;
    TActorId FileQueue;
    TActorId Consumer;

    explicit TTwoNodeRuntime(size_t fileCount, ui64 batchObjectCountLimit = 1)
        : Runtime(/* nodeCount */ 2)
    {
        // The interconnect mock resolves the peer through the nameservice before it opens a session.
        auto nameserverTable = MakeIntrusive<TTableNameserverSetup>();
        for (ui32 nodeIndex = 0; nodeIndex < 2; ++nodeIndex) {
            nameserverTable->StaticNodeTable[Runtime.GetNodeId(nodeIndex)] =
                TTableNameserverSetup::TNodeInfo("::1", "::1", 12001 + nodeIndex);
        }
        for (ui32 nodeIndex = 0; nodeIndex < 2; ++nodeIndex) {
            Runtime.AddLocalService(GetNameserviceActorId(),
                TActorSetupCmd(CreateNameserverTable(nameserverTable), TMailboxType::Simple, 0), nodeIndex);
        }
        Runtime.Initialize();
        RegisterKikimrLogComponents(Runtime, 2);
        // Keep all scheduled events: TRetryEventsQueue drives reconnects through zero-delay TEvRetry.
        Runtime.SetScheduledEventFilter([](TTestActorRuntimeBase&, TAutoPtr<IEventHandle>&, TDuration, TInstant&) {
            return false;
        });
        Edge = Runtime.AllocateEdgeActor(0);
        // Runtime listing: the queue lives on another node than the read actor, so TRetryEventsQueue is in
        // remote (non-LocalRecipient) mode and the interconnect mock sits in between.
        FileQueue = Runtime.Register(MakeFileQueue(fileCount, batchObjectCountLimit), /* nodeIndex */ 1);
        Consumer = Runtime.Register(new TFileQueueConsumer(FileQueue, Edge), /* nodeIndex */ 0);
    }

    void RequestBatch() {
        Runtime.Send(new IEventHandle(Consumer, Edge, new TEvTest::TEvRequestBatch()), 0, /* viaActorSystem */ true);
    }

    TEvTest::TEvBatchAccepted::TPtr GrabAccepted() {
        auto ev = Runtime.GrabEdgeEvent<TEvTest::TEvBatchAccepted>(Edge);
        UNIT_ASSERT(ev);
        return ev;
    }

    TEvTest::TEvDuplicateDropped::TPtr GrabDuplicateDropped() {
        auto ev = Runtime.GrabEdgeEvent<TEvTest::TEvDuplicateDropped>(Edge);
        UNIT_ASSERT(ev);
        return ev;
    }

    // Simulate an interconnect session break between the two nodes (network blip; both actors stay alive).
    void BreakInterconnectSession() {
        Runtime.Send(new IEventHandle(Runtime.GetInterconnectProxy(0, 1), Edge, new TEvInterconnect::TEvDisconnect()), 0, /* viaActorSystem */ true);
    }
};

} // namespace

Y_UNIT_TEST_SUITE(TS3FileQueueActorTest) {

    // Server-side contract: a repeated TEvGetNextBatch with the same SeqNo is what TRetryEventsQueue
    // sends after a reconnect. The queue must answer it with the batch it already handed out for that
    // SeqNo, not consume and hand out a new one.
    Y_UNIT_TEST(DuplicateGetNextBatchReturnsSameBatch) {
        constexpr size_t fileCount = 3;
        TTestActorRuntimeBase runtime;
        runtime.Initialize();
        RegisterKikimrLogComponents(runtime, 1);
        const auto edge = runtime.AllocateEdgeActor();
        const auto fileQueue = runtime.Register(MakeFileQueue(fileCount, /* batchObjectCountLimit */ 1));

        runtime.Send(new IEventHandle(fileQueue, edge, MakeGetNextBatch(/* seqNo */ 1, /* confirmed */ 0)), 0, /* viaActorSystem */ true);
        auto first = runtime.GrabEdgeEvent<TEvS3Provider::TEvObjectPathBatch>(edge);
        UNIT_ASSERT(first);
        const auto firstPaths = ExtractPaths(first->Get()->Record);
        UNIT_ASSERT_VALUES_EQUAL(firstPaths.size(), 1);

        // Same SeqNo again == retry of a request whose answer was lost (or not yet confirmed) on a reconnect.
        runtime.Send(new IEventHandle(fileQueue, edge, MakeGetNextBatch(/* seqNo */ 1, /* confirmed */ 1)), 0, /* viaActorSystem */ true);
        auto second = runtime.GrabEdgeEvent<TEvS3Provider::TEvObjectPathBatch>(edge);
        UNIT_ASSERT(second);
        UNIT_ASSERT_VALUES_EQUAL(second->Get()->Record.GetTransportMeta().GetSeqNo(), 1);
        UNIT_ASSERT_VALUES_EQUAL_C(JoinSeq(",", ExtractPaths(second->Get()->Record)), JoinSeq(",", firstPaths),
            "retried request with SeqNo=1 was served with a different batch: the first one is lost");
    }

    // End-to-end reproduction with the real TRetryEventsQueue on the consumer side and a real
    // interconnect session break in between:
    //   1. consumer requests batch #1 and receives it (Events still holds request #1: it is confirmed
    //      only by the answer to request #2, because the queue just echoes TransportMeta back);
    //   2. the session breaks; TRetryEventsQueue reconnects and re-sends request #1;
    //   3. the queue pops *another* object for it; the consumer rejects that answer as a duplicate
    //      of SeqNo=1 and drops it;
    //   4. the remaining requests drain the queue, which reports NoMoreFiles as if nothing happened.
    // The read actor would count ListedFiles from accepted batches only and finish the query successfully
    // with a file missing.
    Y_UNIT_TEST(BatchIsSilentlyLostAfterInterconnectReconnect) {
        constexpr size_t fileCount = 3;
        TTwoNodeRuntime env(fileCount);

        std::set<TString> received;

        env.RequestBatch();
        auto first = env.GrabAccepted();
        UNIT_ASSERT_VALUES_EQUAL(first->Get()->SeqNo, 1);
        UNIT_ASSERT_VALUES_EQUAL(first->Get()->Paths.size(), 1);
        UNIT_ASSERT(!first->Get()->NoMoreFiles);
        received.insert(first->Get()->Paths.begin(), first->Get()->Paths.end());

        env.BreakInterconnectSession();

        // The reconnect re-sends request #1; whatever the queue answers is rejected by OnEventReceived.
        auto dropped = env.GrabDuplicateDropped();
        UNIT_ASSERT_VALUES_EQUAL(dropped->Get()->SeqNo, 1);
        std::vector<TString> lostOnDuplicate;
        for (const auto& path : dropped->Get()->Paths) {
            if (!received.contains(path)) {
                lostOnDuplicate.push_back(path);
            }
        }

        // Drain the rest of the queue exactly like the read actor would.
        bool noMoreFiles = false;
        for (ui64 seqNo = 2; !noMoreFiles; ++seqNo) {
            env.RequestBatch();
            auto batch = env.GrabAccepted();
            UNIT_ASSERT_VALUES_EQUAL(batch->Get()->SeqNo, seqNo);
            received.insert(batch->Get()->Paths.begin(), batch->Get()->Paths.end());
            noMoreFiles = batch->Get()->NoMoreFiles;
            UNIT_ASSERT_C(seqNo <= fileCount + 1, "queue never reported NoMoreFiles");
        }

        UNIT_ASSERT_VALUES_EQUAL_C(JoinSeq(",", received), JoinSeq(",", AllFiles(fileCount)),
            "queue reported NoMoreFiles but not every file reached the consumer; objects served for the "
            "re-sent SeqNo=1 and dropped as duplicates: " << JoinSeq(",", lostOnDuplicate));
        UNIT_ASSERT_C(lostOnDuplicate.empty(),
            "duplicate answer to SeqNo=1 carried never-delivered objects: " << JoinSeq(",", lostOnDuplicate));
    }
}
