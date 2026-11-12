#ifndef MBP_CLOCK_H
#define MBP_CLOCK_H

#include <stdint.h>

#include "mbp.h"

MbpStatus MbpClock_MonoMs(uint64_t *nowMs);
MbpStatus MbpClock_WallMs(uint64_t *nowMs);

#endif // MBP_CLOCK_H
