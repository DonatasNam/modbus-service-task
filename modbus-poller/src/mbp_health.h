#ifndef MBP_HEALTH_H
#define MBP_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#include "mbp.h"

#define MBP_HEALTH_BACKOFF_MIN_MS 1000u
#define MBP_HEALTH_BACKOFF_MAX_MS 30000u

typedef enum {
    MbpHealthStateConnecting = 0,
    MbpHealthStateOnline,
    MbpHealthStateError,
    MbpHealthStateOffline,
    MbpHealthStateDisabled,
    MbpHealthStateCount,
} MbpHealthState;

typedef enum {
    MbpHealthStepDone = 0,
    MbpHealthStepPointFault,                // no retry; the point keeps the error, the link is fine
    MbpHealthStepRetry,                     // retry on the same link
    MbpHealthStepReconnect,                 // reconnect first, so a late reply is never read as the retry's
    MbpHealthStepExhausted,                 // a link fault with no attempt left
} MbpHealthStep;

// Times are wall-clock epoch ms, 0 = never.
typedef struct {
    MbpHealthState state;
    uint64_t sinceMs;
    uint64_t lastOkMs;
    MbpStatus lastError;
    uint64_t readsOk;
    uint64_t readsFailed;
    uint64_t reconnects;
    uint32_t failedConnects;                // consecutive
    uint32_t backoffMs;                     // the wait the next failure gets
    uint32_t waitMs;                        // the wait after the latest failure, before the next connect
    bool hasConnected;
} MbpHealth;

MbpStatus MbpHealth_Reset(MbpHealth *health, uint64_t nowWallMs);
MbpStatus MbpHealth_Disable(MbpHealth *health, uint64_t nowWallMs);
MbpStatus MbpHealth_OnConnect(MbpHealth *health, MbpStatus result, uint8_t connectRetries, uint64_t nowWallMs);
MbpStatus MbpHealth_NextStep(MbpStatus result, uint8_t attempt, uint8_t retries, MbpHealthStep *step);
MbpStatus MbpHealth_OnRead(MbpHealth *health, MbpStatus result, uint64_t nowWallMs);
MbpStatus MbpHealth_StateName(MbpHealthState state, const char **name);

#endif // MBP_HEALTH_H
