#include <stdint.h>

#include "mbp_clock.h"
#include "mbp_sched.h"
#include "mbp_test_check.h"

static void TestPickNext(void);
static void TestAdvance(void);
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
    TestClock();
    return MBP_TEST_RESULT();
}
