// Internal to modbus-poller. External users include mbp.h.
#ifndef MBP_SCHED_H
#define MBP_SCHED_H

#include <stdint.h>

#include "mbp.h"

MbpStatus MbpSched_PickNext(const uint64_t *dueMs, uint16_t count, uint64_t nowMs, uint16_t *index, uint64_t *waitMs);
MbpStatus MbpSched_Advance(uint64_t *dueMs, uint32_t cycleMs, uint64_t nowMs);

#endif // MBP_SCHED_H
