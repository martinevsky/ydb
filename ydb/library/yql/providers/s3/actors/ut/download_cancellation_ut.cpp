#include <ydb/library/yql/providers/s3/actors/download_cancellation.h>

#include <library/cpp/testing/unittest/registar.h>

#include <atomic>
#include <latch>
#include <thread>

namespace NYql::NDq {

Y_UNIT_TEST_SUITE(S3DownloadCancellation) {
    Y_UNIT_TEST(CancelBeforePublication) {
        TDownloadCancellation cancellation;
        ui32 calls = 0;
        cancellation.Cancel();
        cancellation.Cancel();
        cancellation.Publish([&](const TIssue&) { ++calls; });
        cancellation.Cancel();
        UNIT_ASSERT(cancellation.IsCancelled());
        UNIT_ASSERT_VALUES_EQUAL(calls, 1);
    }

    Y_UNIT_TEST(CancelAfterPublication) {
        TDownloadCancellation cancellation;
        ui32 calls = 0;
        cancellation.Publish([&](const TIssue&) { ++calls; });
        UNIT_ASSERT(!cancellation.IsCancelled());
        UNIT_ASSERT_VALUES_EQUAL(calls, 0);
        cancellation.Cancel();
        cancellation.Cancel();
        UNIT_ASSERT_VALUES_EQUAL(calls, 1);
    }

    Y_UNIT_TEST(ConcurrentPublicationAndCancellation) {
        for (ui32 attempt = 0; attempt < 64; ++attempt) {
            TDownloadCancellation cancellation;
            std::atomic<ui32> calls{0};
            std::latch start{3};
            std::thread publisher([&] {
                start.arrive_and_wait();
                cancellation.Publish([&](const TIssue&) { ++calls; });
            });
            std::thread canceller([&] {
                start.arrive_and_wait();
                cancellation.Cancel();
                cancellation.Cancel();
            });
            start.arrive_and_wait();
            publisher.join();
            canceller.join();
            UNIT_ASSERT(cancellation.IsCancelled());
            UNIT_ASSERT_VALUES_EQUAL(calls.load(), 1);
        }
    }

    Y_UNIT_TEST(ReentrantHookAfterPublication) {
        TDownloadCancellation cancellation;
        ui32 calls = 0;
        cancellation.Publish([&](const TIssue&) {
            ++calls;
            UNIT_ASSERT(cancellation.IsCancelled());
            cancellation.Cancel();
        });
        cancellation.Cancel();
        UNIT_ASSERT_VALUES_EQUAL(calls, 1);
    }

    Y_UNIT_TEST(ReentrantHookBeforePublication) {
        TDownloadCancellation cancellation;
        ui32 calls = 0;
        cancellation.Cancel();
        cancellation.Publish([&](const TIssue&) {
            ++calls;
            UNIT_ASSERT(cancellation.IsCancelled());
            cancellation.Cancel();
        });
        UNIT_ASSERT_VALUES_EQUAL(calls, 1);
    }

    Y_UNIT_TEST(RetryReplacesCompletedAttemptHook) {
        TDownloadCancellation cancellation;
        ui32 previousCalls = 0;
        ui32 currentCalls = 0;
        cancellation.Publish([&](const TIssue&) { ++previousCalls; });
        cancellation.Publish([&](const TIssue&) { ++currentCalls; });
        cancellation.Cancel();
        UNIT_ASSERT_VALUES_EQUAL(previousCalls, 0);
        UNIT_ASSERT_VALUES_EQUAL(currentCalls, 1);
    }

    Y_UNIT_TEST(EmptyHook) {
        TDownloadCancellation cancellation;
        cancellation.Publish({});
        cancellation.Cancel();
        cancellation.Publish({});
        UNIT_ASSERT(cancellation.IsCancelled());
    }
}

} // namespace NYql::NDq
