#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mbp_defs.h"
#include "mbp_health.h"

typedef struct {
    MbpStatus result;
    MbpHealthStep step;
} ResultStep;

static MbpHealthStep Classify(MbpStatus result);
static void SetState(MbpHealth *health, MbpHealthState state, uint64_t nowWallMs);
static void BackOff(MbpHealth *health);

// One attempt's result to its next step while attempts remain. Anything not listed is a link fault that reconnects.
static const ResultStep resultSteps[] = {
    { .result = MbpStatusOk, .step = MbpHealthStepDone },
    { .result = MbpStatusDecNotFinite, .step = MbpHealthStepPointFault },
    { .result = MbpStatusDecType, .step = MbpHealthStepPointFault },
    { .result = MbpStatusLinkExcIllegalFunction, .step = MbpHealthStepPointFault },
    { .result = MbpStatusLinkExcIllegalAddress, .step = MbpHealthStepPointFault },
    { .result = MbpStatusLinkExcIllegalValue, .step = MbpHealthStepPointFault },
    { .result = MbpStatusLinkExcDeviceFailure, .step = MbpHealthStepPointFault },
    { .result = MbpStatusLinkExcOther, .step = MbpHealthStepPointFault },
    { .result = MbpStatusLinkExcBusy, .step = MbpHealthStepRetry },
    { .result = MbpStatusLinkExcGatewayPath, .step = MbpHealthStepRetry },
    { .result = MbpStatusLinkExcGatewayTarget, .step = MbpHealthStepRetry },
    { .result = MbpStatusLinkTimeout, .step = MbpHealthStepReconnect },
    { .result = MbpStatusLinkBadData, .step = MbpHealthStepReconnect },
    { .result = MbpStatusLinkIo, .step = MbpHealthStepReconnect },
};

// wire protocol v1: state names; clients store these; additive only
static const char *const stateNames[MbpHealthStateCount] = {
    [MbpHealthStateConnecting] = "connecting",
    [MbpHealthStateOnline] = "online",
    [MbpHealthStateError] = "error",
    [MbpHealthStateOffline] = "offline",
    [MbpHealthStateDisabled] = "disabled",
};

static MbpHealthStep Classify(MbpStatus result) {
    for (size_t i = 0u; i < (sizeof(resultSteps) / sizeof(resultSteps[0])); i++) {
        if (resultSteps[i].result == result) {
            return resultSteps[i].step;
        }
    }
    return MbpHealthStepReconnect;
}

static void SetState(MbpHealth *health, MbpHealthState state, uint64_t nowWallMs) {
    if (health->state != state) {
        health->state = state;
        health->sinceMs = nowWallMs;
    }
}

static void BackOff(MbpHealth *health) {
    health->waitMs = health->backoffMs;
    health->backoffMs = ((2u * health->backoffMs) < MBP_HEALTH_BACKOFF_MAX_MS) ? (2u * health->backoffMs) : MBP_HEALTH_BACKOFF_MAX_MS;
}

MbpStatus MbpHealth_Reset(MbpHealth *health, uint64_t nowWallMs) {
    if (health == NULL) {
        return MbpStatusParam;
    }
    *health = (MbpHealth) { .state = MbpHealthStateConnecting, .sinceMs = nowWallMs, .lastError = MbpStatusOk, .backoffMs = MBP_HEALTH_BACKOFF_MIN_MS, .waitMs = MBP_HEALTH_BACKOFF_MIN_MS };
    return MbpStatusOk;
}

MbpStatus MbpHealth_Disable(MbpHealth *health, uint64_t nowWallMs) {
    if (health == NULL) {
        return MbpStatusParam;
    }
    *health = (MbpHealth) { .state = MbpHealthStateDisabled, .sinceMs = nowWallMs, .lastError = MbpStatusOk, .backoffMs = MBP_HEALTH_BACKOFF_MIN_MS, .waitMs = MBP_HEALTH_BACKOFF_MIN_MS };
    return MbpStatusOk;
}

MbpStatus MbpHealth_OnConnect(MbpHealth *health, MbpStatus result, uint8_t connectRetries, uint64_t nowWallMs) {
    if (health == NULL) {
        return MbpStatusParam;
    }
    if (result == MbpStatusOk) {
        if (health->hasConnected) {
            health->reconnects++;
        }
        health->hasConnected = true;
        health->failedConnects = 0u;
        if (health->state == MbpHealthStateOffline) {
            SetState(health, MbpHealthStateConnecting, nowWallMs);
        }
        return MbpStatusOk;
    }
    health->lastError = result;
    if (health->failedConnects < UINT32_MAX) {
        health->failedConnects++;
    }
    BackOff(health);
    if (health->failedConnects > connectRetries) {
        SetState(health, MbpHealthStateOffline, nowWallMs);
    }
    return MbpStatusOk;
}

// attempt counts from 0; a read has 1 + retries attempts.
MbpStatus MbpHealth_NextStep(MbpStatus result, uint8_t attempt, uint8_t retries, MbpHealthStep *step) {
    if (step == NULL) {
        return MbpStatusParam;
    }
    const MbpHealthStep next = Classify(result);
    const bool isRetry = (next == MbpHealthStepRetry) || (next == MbpHealthStepReconnect);
    *step = (isRetry && (attempt >= retries)) ? MbpHealthStepExhausted : next;
    return MbpStatusOk;
}

MbpStatus MbpHealth_OnRead(MbpHealth *health, MbpStatus result, uint64_t nowWallMs) {
    if (health == NULL) {
        return MbpStatusParam;
    }
    switch (Classify(result)) {
    case MbpHealthStepDone:
        health->readsOk++;
        health->lastOkMs = nowWallMs;
        health->backoffMs = MBP_HEALTH_BACKOFF_MIN_MS;
        SetState(health, MbpHealthStateOnline, nowWallMs);
        break;
    case MbpHealthStepPointFault:
        health->readsFailed++;
        SetState(health, MbpHealthStateOnline, nowWallMs);
        break;
    case MbpHealthStepRetry:
    case MbpHealthStepReconnect:
    case MbpHealthStepExhausted:
        health->readsFailed++;
        health->lastError = result;
        BackOff(health);
        SetState(health, MbpHealthStateError, nowWallMs);
        break;
    }
    return MbpStatusOk;
}

MbpStatus MbpHealth_StateName(MbpHealthState state, const char **name) {
    if ((name == NULL) || ((unsigned) state >= (unsigned) MbpHealthStateCount)) {
        return MbpStatusParam;
    }
    *name = stateNames[state];
    return MbpStatusOk;
}
