#include <stdint.h>

#include "mbp_defs.h"
#include "mbp_sched.h"

static int64_t DiffMs(uint64_t laterMs, uint64_t earlierMs);

// Monotonic milliseconds never wrap within an uptime; the signed difference keeps comparisons right if they did.
static int64_t DiffMs(uint64_t laterMs, uint64_t earlierMs) {
    return (int64_t) (laterMs - earlierMs);
}

// The earliest due point wins, the lowest index on a tie; a point already due waits 0.
MbpStatus MbpSched_PickNext(const uint64_t *dueMs, uint16_t count, uint64_t nowMs, uint16_t *index, uint64_t *waitMs) {
    if ((dueMs == NULL) || (count == 0u) || (index == NULL) || (waitMs == NULL)) {
        return MbpStatusParam;
    }
    uint16_t best = 0u;
    for (uint16_t i = 1u; i < count; i++) {
        if (DiffMs(dueMs[i], dueMs[best]) < 0) {
            best = i;
        }
    }
    const int64_t leftMs = DiffMs(dueMs[best], nowMs);
    *index = best;
    *waitMs = (leftMs > 0) ? (uint64_t) leftMs : 0u;
    return MbpStatusOk;
}

// Next due = due + cycle; after an overrun, now + cycle, so a late station never bursts to catch up.
MbpStatus MbpSched_Advance(uint64_t *dueMs, uint32_t cycleMs, uint64_t nowMs) {
    if ((dueMs == NULL) || (cycleMs == 0u)) {
        return MbpStatusParam;
    }
    const uint64_t nextMs = *dueMs + cycleMs;
    *dueMs = (DiffMs(nextMs, nowMs) > 0) ? nextMs : (nowMs + cycleMs);
    return MbpStatusOk;
}

MbpStatus MbpSched_MakeAllDue(uint64_t *dueMs, uint16_t count, uint64_t nowMs) {
    if (dueMs == NULL) {
        return MbpStatusParam;
    }
    for (uint16_t i = 0u; i < count; i++) {
        dueMs[i] = nowMs;
    }
    return MbpStatusOk;
}

// A link loss leaves every point stale, so every point is due by now and is read right after the reconnect, whatever
// its cycle. A point already overdue keeps its earlier due time, so it stays ahead of the points pulled in: with any
// number of failing points, every overdue point is reached in turn and none starves. The point whose read failed keeps
// its own next due time, at most retryMs away, so it comes after all the others and a long-cycle point is retried.
MbpStatus MbpSched_RearmAfterLoss(uint64_t *dueMs, uint16_t count, uint16_t failedIndex, uint64_t retryMs, uint64_t nowMs) {
    if ((dueMs == NULL) || (failedIndex >= count)) {
        return MbpStatusParam;
    }
    for (uint16_t i = 0u; i < count; i++) {
        if ((i != failedIndex) && (DiffMs(dueMs[i], nowMs) > 0)) {
            dueMs[i] = nowMs;
        }
    }
    const uint64_t latestRetryMs = nowMs + retryMs;
    if (DiffMs(dueMs[failedIndex], latestRetryMs) > 0) {
        dueMs[failedIndex] = latestRetryMs;
    }
    return MbpStatusOk;
}
