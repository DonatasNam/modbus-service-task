#ifndef MBP_SCHED_H
#define MBP_SCHED_H

#include <stdint.h>

#include "mbp.h"

MbpStatus MbpSched_PickNext(const uint64_t *dueMs, uint16_t count, uint64_t nowMs, uint16_t *index, uint64_t *waitMs);
MbpStatus MbpSched_Advance(uint64_t *dueMs, uint32_t cycleMs, uint64_t nowMs);
MbpStatus MbpSched_MakeAllDue(uint64_t *dueMs, uint16_t count, uint64_t nowMs);
MbpStatus MbpSched_RearmAfterLoss(uint64_t *dueMs, uint16_t count, uint16_t failedIndex, uint64_t retryMs, uint64_t nowMs);

#endif // MBP_SCHED_H
