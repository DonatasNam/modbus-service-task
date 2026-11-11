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
