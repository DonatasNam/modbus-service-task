#include <stdint.h>

#include "mbp_defs.h"
#include "mbp_sched.h"

static int64_t DiffMs(uint64_t laterMs, uint64_t earlierMs);

static int64_t DiffMs(uint64_t laterMs, uint64_t earlierMs) {
    return (int64_t) (laterMs - earlierMs);
}

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
