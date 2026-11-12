#include <stdint.h>

#include "mbp_clock.h"
#include "mbp_sched.h"
#include "mbp_test_check.h"

static void TestPickNext(void);
static void TestAdvance(void);
static void TestMakeAllDue(void);
static void TestRearmAfterLoss(void);
static void TestTwoFailingPointsStarveNothing(void);
static void TestClock(void);

static void TestPickNext(void) {
    const uint64_t dueMs[4] = { 5000u, 3000u, 3000u, 9000u };
    uint16_t index = 99u;
    uint64_t waitMs = 99u;
    MBP_CHECK_STATUS(MbpSched_PickNext(dueMs, 4u, 1000u, &index, &waitMs), MbpStatusOk);
    MBP_CHECK(index == 1u);
    MBP_CHECK(waitMs == 2000u);
    MBP_CHECK_STATUS(MbpSched_PickNext(dueMs, 4u, 3000u, &index, &waitMs), MbpStatusOk);
    MBP_CHECK(index == 1u);
    MBP_CHECK(waitMs == 0u);
    MBP_CHECK_STATUS(MbpSched_PickNext(dueMs, 4u, 7000u, &index, &waitMs), MbpStatusOk);
    MBP_CHECK(index == 1u);
    MBP_CHECK(waitMs == 0u);
    MBP_CHECK_STATUS(MbpSched_PickNext(dueMs, 1u, 1000u, &index, &waitMs), MbpStatusOk);
    MBP_CHECK(index == 0u);
    MBP_CHECK(waitMs == 4000u);
    MBP_CHECK_STATUS(MbpSched_PickNext(dueMs, 0u, 1000u, &index, &waitMs), MbpStatusParam);
    MBP_CHECK_STATUS(MbpSched_PickNext(NULL, 1u, 1000u, &index, &waitMs), MbpStatusParam);
}

static void TestAdvance(void) {
    uint64_t dueMs = 1000u;
    MBP_CHECK_STATUS(MbpSched_Advance(&dueMs, 1000u, 1200u), MbpStatusOk);
    MBP_CHECK(dueMs == 2000u);
    MBP_CHECK_STATUS(MbpSched_Advance(&dueMs, 1000u, 2900u), MbpStatusOk);
    MBP_CHECK(dueMs == 3000u);
    MBP_CHECK_STATUS(MbpSched_Advance(&dueMs, 1000u, 7500u), MbpStatusOk);
    MBP_CHECK(dueMs == 8500u);
    MBP_CHECK_STATUS(MbpSched_Advance(&dueMs, 1000u, 9500u), MbpStatusOk);
    MBP_CHECK(dueMs == 10500u);
    MBP_CHECK_STATUS(MbpSched_Advance(&dueMs, 0u, 9500u), MbpStatusParam);
    MBP_CHECK_STATUS(MbpSched_Advance(NULL, 1000u, 9500u), MbpStatusParam);
}

static void TestMakeAllDue(void) {
    uint64_t dueMs[3] = { 10u, 20u, 30u };
    MBP_CHECK_STATUS(MbpSched_MakeAllDue(dueMs, 3u, 500u), MbpStatusOk);
    MBP_CHECK((dueMs[0] == 500u) && (dueMs[1] == 500u) && (dueMs[2] == 500u));
    MBP_CHECK_STATUS(MbpSched_MakeAllDue(NULL, 3u, 500u), MbpStatusParam);
}

// The failed point comes after every other point; an overdue point keeps its place ahead of the points pulled in.
static void TestRearmAfterLoss(void) {
    uint64_t dueMs[4] = { 9000u, 1000u + 3600000u, 7000u, 400u };
    uint16_t index = 99u;
    uint64_t waitMs = 99u;
    MBP_CHECK_STATUS(MbpSched_RearmAfterLoss(dueMs, 4u, 1u, 30000u, 1000u), MbpStatusOk);
    MBP_CHECK((dueMs[0] == 1000u) && (dueMs[2] == 1000u) && (dueMs[3] == 400u));
    MBP_CHECK(dueMs[1] == 31000u);
    MBP_CHECK_STATUS(MbpSched_PickNext(dueMs, 4u, 1000u, &index, &waitMs), MbpStatusOk);
    MBP_CHECK(index == 3u);

    dueMs[1] = 2000u;
    MBP_CHECK_STATUS(MbpSched_RearmAfterLoss(dueMs, 4u, 1u, 30000u, 1000u), MbpStatusOk);
    MBP_CHECK(dueMs[1] == 2000u);
    MBP_CHECK_STATUS(MbpSched_RearmAfterLoss(dueMs, 4u, 4u, 30000u, 1000u), MbpStatusParam);
}

// Two points that keep failing: every other point is still read, in the order its due time sets.
static void TestTwoFailingPointsStarveNothing(void) {
    uint64_t dueMs[6] = { 0u, 0u, 0u, 0u, 0u, 0u };
    uint32_t reads[6] = { 0u, 0u, 0u, 0u, 0u, 0u };
    uint64_t nowMs = 1000u;
    for (unsigned step = 0u; step < 200u; step++) {
        uint16_t index = 0u;
        uint64_t waitMs = 0u;
        MBP_CHECK_STATUS(MbpSched_PickNext(dueMs, 6u, nowMs, &index, &waitMs), MbpStatusOk);
        nowMs += waitMs + 10u;
        reads[index]++;
        MBP_CHECK_STATUS(MbpSched_Advance(&dueMs[index], 1000u, nowMs), MbpStatusOk);
        if ((index == 1u) || (index == 3u)) {
            MBP_CHECK_STATUS(MbpSched_RearmAfterLoss(dueMs, 6u, index, 30000u, nowMs), MbpStatusOk);
        }
    }
    for (uint16_t i = 0u; i < 6u; i++) {
        MBP_CHECK(reads[i] >= (((i == 1u) || (i == 3u)) ? 1u : 10u));
    }
}

static void TestClock(void) {
    uint64_t firstMs = 0u;
    uint64_t secondMs = 0u;
    uint64_t wallMs = 0u;
    MBP_CHECK_STATUS(MbpClock_MonoMs(&firstMs), MbpStatusOk);
    MBP_CHECK_STATUS(MbpClock_MonoMs(&secondMs), MbpStatusOk);
    MBP_CHECK(secondMs >= firstMs);
    MBP_CHECK_STATUS(MbpClock_WallMs(&wallMs), MbpStatusOk);
    MBP_CHECK(wallMs > UINT64_C(1600000000000));
    MBP_CHECK_STATUS(MbpClock_MonoMs(NULL), MbpStatusParam);
}

int main(void) {
    TestPickNext();
    TestAdvance();
    TestMakeAllDue();
    TestRearmAfterLoss();
    TestTwoFailingPointsStarveNothing();
    TestClock();
    return MBP_TEST_RESULT();
}
