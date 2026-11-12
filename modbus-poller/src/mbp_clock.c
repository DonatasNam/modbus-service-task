#include <stdint.h>
#include <time.h>

#include "mbp_clock.h"
#include "mbp_defs.h"

static MbpStatus ReadMs(clockid_t clock, uint64_t *nowMs);

static MbpStatus ReadMs(clockid_t clock, uint64_t *nowMs) {
    struct timespec now;
    if (nowMs == NULL) {
        return MbpStatusParam;
    }
    if (clock_gettime(clock, &now) != 0) {
        return MbpStatusOs;
    }
    *nowMs = ((uint64_t) now.tv_sec * MBP_MS_PER_SEC) + ((uint64_t) now.tv_nsec / MBP_NS_PER_MS);
    return MbpStatusOk;
}

MbpStatus MbpClock_MonoMs(uint64_t *nowMs) {
    return ReadMs(CLOCK_MONOTONIC, nowMs);
}

MbpStatus MbpClock_WallMs(uint64_t *nowMs) {
    return ReadMs(CLOCK_REALTIME, nowMs);
}
