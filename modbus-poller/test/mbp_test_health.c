#include <stdint.h>
#include <string.h>

#include "mbp_health.h"
#include "mbp_test_check.h"

#define CONNECT_RETRIES 3u
#define REQUEST_RETRIES 3u

typedef struct {
    MbpStatus result;
    uint8_t attempt;
    uint8_t retries;
    MbpHealthStep expected;
} StepCase;

static void TestConnectFailures(void);
static void TestReadCycle(void);
static void TestBackoffCap(void);
static void TestNextStep(void);
static void TestStateName(void);

static const StepCase stepCases[] = {
    { .result = MbpStatusOk, .attempt = 0u, .retries = REQUEST_RETRIES, .expected = MbpHealthStepDone },
    { .result = MbpStatusLinkTimeout, .attempt = 0u, .retries = REQUEST_RETRIES, .expected = MbpHealthStepReconnect },
    { .result = MbpStatusLinkBadData, .attempt = 2u, .retries = REQUEST_RETRIES, .expected = MbpHealthStepReconnect },
    { .result = MbpStatusLinkIo, .attempt = 3u, .retries = REQUEST_RETRIES, .expected = MbpHealthStepExhausted },
    { .result = MbpStatusLinkTimeout, .attempt = 0u, .retries = 0u, .expected = MbpHealthStepExhausted },
    { .result = MbpStatusLinkExcBusy, .attempt = 0u, .retries = REQUEST_RETRIES, .expected = MbpHealthStepRetry },
    { .result = MbpStatusLinkExcGatewayPath, .attempt = 1u, .retries = REQUEST_RETRIES, .expected = MbpHealthStepRetry },
    { .result = MbpStatusLinkExcGatewayTarget, .attempt = 3u, .retries = REQUEST_RETRIES, .expected = MbpHealthStepExhausted },
    { .result = MbpStatusLinkExcIllegalFunction, .attempt = 0u, .retries = REQUEST_RETRIES, .expected = MbpHealthStepPointFault },
    { .result = MbpStatusLinkExcIllegalAddress, .attempt = 3u, .retries = REQUEST_RETRIES, .expected = MbpHealthStepPointFault },
    { .result = MbpStatusLinkExcIllegalValue, .attempt = 0u, .retries = 0u, .expected = MbpHealthStepPointFault },
    { .result = MbpStatusLinkExcDeviceFailure, .attempt = 0u, .retries = REQUEST_RETRIES, .expected = MbpHealthStepPointFault },
    { .result = MbpStatusLinkExcOther, .attempt = 0u, .retries = REQUEST_RETRIES, .expected = MbpHealthStepPointFault },
    { .result = MbpStatusLinkConnect, .attempt = 0u, .retries = REQUEST_RETRIES, .expected = MbpHealthStepReconnect },
};

static void TestConnectFailures(void) {
    MbpHealth health;
    MBP_CHECK_STATUS(MbpHealth_Reset(&health, 100u), MbpStatusOk);
    MBP_CHECK(health.state == MbpHealthStateConnecting);
    MBP_CHECK(health.sinceMs == 100u);
    const uint32_t expectedWaits[] = { 1000u, 2000u, 4000u, 8000u };
    for (uint32_t i = 0u; i < 4u; i++) {
        MBP_CHECK_STATUS(MbpHealth_OnConnect(&health, MbpStatusLinkConnect, CONNECT_RETRIES, 200u + i), MbpStatusOk);
        MBP_CHECK(health.waitMs == expectedWaits[i]);
        MBP_CHECK(health.lastError == MbpStatusLinkConnect);
        MBP_CHECK(health.state == ((i < CONNECT_RETRIES) ? MbpHealthStateConnecting : MbpHealthStateOffline));
    }
    MBP_CHECK(health.sinceMs == 203u);
    MBP_CHECK_STATUS(MbpHealth_OnConnect(&health, MbpStatusOk, CONNECT_RETRIES, 300u), MbpStatusOk);
    MBP_CHECK(health.state == MbpHealthStateConnecting);
    MBP_CHECK(health.sinceMs == 300u);
    MBP_CHECK(health.failedConnects == 0u);
    MBP_CHECK(health.reconnects == 0u);
    MBP_CHECK(health.backoffMs == 16000u);
}

static void TestReadCycle(void) {
    MbpHealth health;
    MBP_CHECK_STATUS(MbpHealth_Reset(&health, 100u), MbpStatusOk);
    MBP_CHECK_STATUS(MbpHealth_OnConnect(&health, MbpStatusOk, CONNECT_RETRIES, 110u), MbpStatusOk);
    MBP_CHECK_STATUS(MbpHealth_OnRead(&health, MbpStatusOk, 120u), MbpStatusOk);
    MBP_CHECK(health.state == MbpHealthStateOnline);
    MBP_CHECK(health.sinceMs == 120u);
    MBP_CHECK(health.lastOkMs == 120u);
    MBP_CHECK(health.readsOk == 1u);

    MBP_CHECK_STATUS(MbpHealth_OnRead(&health, MbpStatusLinkExcIllegalAddress, 130u), MbpStatusOk);
    MBP_CHECK(health.state == MbpHealthStateOnline);
    MBP_CHECK(health.sinceMs == 120u);
    MBP_CHECK(health.readsFailed == 1u);
    MBP_CHECK(health.lastError == MbpStatusOk);

    MBP_CHECK_STATUS(MbpHealth_OnRead(&health, MbpStatusLinkTimeout, 140u), MbpStatusOk);
    MBP_CHECK(health.state == MbpHealthStateError);
    MBP_CHECK(health.sinceMs == 140u);
    MBP_CHECK(health.lastError == MbpStatusLinkTimeout);
    MBP_CHECK(health.waitMs == 1000u);

    MBP_CHECK_STATUS(MbpHealth_OnConnect(&health, MbpStatusOk, CONNECT_RETRIES, 150u), MbpStatusOk);
    MBP_CHECK(health.reconnects == 1u);
    MBP_CHECK(health.state == MbpHealthStateError);
    MBP_CHECK_STATUS(MbpHealth_OnRead(&health, MbpStatusLinkTimeout, 160u), MbpStatusOk);
    MBP_CHECK(health.waitMs == 2000u);
    MBP_CHECK(health.sinceMs == 140u);

    MBP_CHECK_STATUS(MbpHealth_OnRead(&health, MbpStatusOk, 170u), MbpStatusOk);
    MBP_CHECK(health.state == MbpHealthStateOnline);
    MBP_CHECK(health.lastError == MbpStatusLinkTimeout);
    MBP_CHECK_STATUS(MbpHealth_OnRead(&health, MbpStatusLinkExcGatewayTarget, 180u), MbpStatusOk);
    MBP_CHECK(health.state == MbpHealthStateError);
    MBP_CHECK(health.waitMs == 1000u);
    MBP_CHECK(health.readsOk == 2u);
    MBP_CHECK(health.readsFailed == 4u);
}

static void TestBackoffCap(void) {
    MbpHealth health;
    MBP_CHECK_STATUS(MbpHealth_Reset(&health, 0u), MbpStatusOk);
    for (uint32_t i = 0u; i < 10u; i++) {
        MBP_CHECK_STATUS(MbpHealth_OnRead(&health, MbpStatusLinkIo, i), MbpStatusOk);
    }
    MBP_CHECK(health.waitMs == MBP_HEALTH_BACKOFF_MAX_MS);
    MBP_CHECK(health.backoffMs == MBP_HEALTH_BACKOFF_MAX_MS);
    MBP_CHECK_STATUS(MbpHealth_Disable(&health, 5u), MbpStatusOk);
    MBP_CHECK(health.state == MbpHealthStateDisabled);
    MBP_CHECK(health.readsFailed == 0u);
}

static void TestNextStep(void) {
    for (size_t i = 0u; i < (sizeof(stepCases) / sizeof(stepCases[0])); i++) {
        MbpHealthStep step = MbpHealthStepDone;
        MBP_CHECK_STATUS(MbpHealth_NextStep(stepCases[i].result, stepCases[i].attempt, stepCases[i].retries, &step), MbpStatusOk);
        MBP_CHECK(step == stepCases[i].expected);
    }
    MBP_CHECK_STATUS(MbpHealth_NextStep(MbpStatusOk, 0u, 0u, NULL), MbpStatusParam);
}

static void TestStateName(void) {
    const char *name = NULL;
    MBP_CHECK_STATUS(MbpHealth_StateName(MbpHealthStateOffline, &name), MbpStatusOk);
    MBP_CHECK((name != NULL) && (strcmp(name, "offline") == 0));
    MBP_CHECK_STATUS(MbpHealth_StateName(MbpHealthStateDisabled, &name), MbpStatusOk);
    MBP_CHECK((name != NULL) && (strcmp(name, "disabled") == 0));
    MBP_CHECK_STATUS(MbpHealth_StateName(MbpHealthStateCount, &name), MbpStatusParam);
}

int main(void) {
    TestConnectFailures();
    TestReadCycle();
    TestBackoffCap();
    TestNextStep();
    TestStateName();
    return MBP_TEST_RESULT();
}
