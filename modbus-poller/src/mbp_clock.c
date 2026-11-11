#include <stdint.h>
#include <time.h>

#include "mbp_clock.h"
#include "mbp_defs.h"

#define MS_PER_SEC UINT64_C(1000)
#define NS_PER_MS 1000000

static MbpStatus ReadMs(clockid_t clock, uint64_t *nowMs);

static MbpStatus ReadMs(clockid_t clock, uint64_t *nowMs) {
    struct timespec now;
    if (nowMs == NULL) {
        return MbpStatusParam;
    }
    if (clock_gettime(clock, &now) != 0) {
        return MbpStatusOs;
    }
    *nowMs = ((uint64_t) now.tv_sec * MS_PER_SEC) + (uint64_t) (now.tv_nsec / NS_PER_MS);
    return MbpStatusOk;
}

MbpStatus MbpClock_MonoMs(uint64_t *nowMs) {
    return ReadMs(CLOCK_MONOTONIC, nowMs);
}

MbpStatus MbpClock_WallMs(uint64_t *nowMs) {
    return ReadMs(CLOCK_REALTIME, nowMs);
}
