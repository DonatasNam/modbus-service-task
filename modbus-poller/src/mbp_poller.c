#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "mbp_clock.h"
#include "mbp_config.h"
#include "mbp_decode.h"
#include "mbp_defs.h"
#include "mbp_health.h"
#include "mbp_link.h"
#include "mbp_poller.h"
#include "mbp_sched.h"
#include "mbp_store.h"

#define MS_PER_SEC UINT64_C(1000)
#define NS_PER_MS UINT64_C(1000000)

// One per enabled station. After MbpPoll_Start a worker belongs to its own thread; the main thread touches it again
// only in MbpPoll_Stop, after the join.
typedef struct {
    const MbpCfgConn *conn;
    const MbpCfgStation *station;
    uint16_t stationIndex;
    MbpLink link;
    MbpHealth health;
    uint64_t dueMs[MBP_MAX_POINTS];
    pthread_t thread;
    bool isStarted;
} Worker;

// mutex guards isStopRequested and stopCond. Lock order: this mutex, then the store's, never the reverse.
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t stopCond;
    bool isStopCondInit;
    bool isStopRequested;
    uint16_t workerCount;
    Worker workers[MBP_MAX_STATIONS];
} Poller;

static MbpStatus OsStatus(int errorNumber);
static MbpStatus CheckStackSize(void);
static MbpStatus InitStopCond(void);
static MbpStatus InitStopCondWith(pthread_condattr_t *attr);
static MbpStatus StartWorker(const MbpCfg *cfg, uint16_t stationIndex);
static MbpStatus CreateThread(Worker *worker);
static MbpStatus CreateThreadWith(Worker *worker, pthread_attr_t *attr);
static MbpStatus RequestStop(void);
static MbpStatus WorkerLoop(Worker *worker);
static MbpStatus WorkerStep(Worker *worker);
static MbpStatus ConnectLink(Worker *worker);
static MbpStatus ReadPoint(Worker *worker, uint16_t pointIndex);
static MbpStatus ReadAttempts(Worker *worker, uint16_t pointIndex, uint16_t *regs);
static MbpStatus Reconnect(Worker *worker);
static MbpStatus FinishRead(Worker *worker, uint16_t pointIndex, MbpStatus result);
static MbpStatus DropLink(Worker *worker);
static MbpStatus MarkFailed(Worker *worker, MbpStatus cause);
static MbpStatus CheckStop(void);
static MbpStatus WaitUntil(uint64_t deadlineMs);
static MbpStatus WaitLocked(uint64_t deadlineMs);
static void LogTransition(const Worker *worker, MbpHealthState before, MbpStatus cause);
static void *WorkerMain(void *arg);

static Poller poller = { .mutex = PTHREAD_MUTEX_INITIALIZER };

// pthread functions return their error number instead of setting errno; errno carries it to MBP_ASSERT_ERRNO.
static MbpStatus OsStatus(int errorNumber) {
    if (errorNumber == 0) {
        return MbpStatusOk;
    }
    errno = errorNumber;
    return MbpStatusOs;
}

// PTHREAD_STACK_MIN is a sysconf() call in newer glibc, so the check runs at start.
static MbpStatus CheckStackSize(void) {
    return ((size_t) MBP_THREAD_STACK_BYTES >= (size_t) PTHREAD_STACK_MIN) ? MbpStatusOk : MbpStatusThread;
}

static MbpStatus InitStopCond(void) {
    pthread_condattr_t attr;
    MbpStatus ret = OsStatus(pthread_condattr_init(&attr));
    MBP_ASSERT_ERRNO(ret);

    ret = InitStopCondWith(&attr);
    (void) pthread_condattr_destroy(&attr);
    return ret;
}

// The deadlines are CLOCK_MONOTONIC milliseconds from MbpClock_MonoMs, so the condvar waits on that clock.
static MbpStatus InitStopCondWith(pthread_condattr_t *attr) {
    MbpStatus ret = OsStatus(pthread_condattr_setclock(attr, CLOCK_MONOTONIC));
    MBP_ASSERT_ERRNO(ret);

    ret = OsStatus(pthread_cond_init(&poller.stopCond, attr));
    MBP_ASSERT_ERRNO(ret);
    poller.isStopCondInit = true;
    poller.isStopRequested = false;
    return ret;
}

// The worker is counted before its first step, so MbpPoll_Stop closes its link whatever step fails.
static MbpStatus StartWorker(const MbpCfg *cfg, uint16_t stationIndex) {
    const MbpCfgStation *station = &cfg->stations[stationIndex];
    const MbpCfgConn *conn = &cfg->conns[station->connIndex];
    Worker *worker = &poller.workers[poller.workerCount];
    uint64_t wallMs = 0u;
    uint64_t nowMs = 0u;
    *worker = (Worker) { .conn = conn, .station = station, .stationIndex = stationIndex, .isStarted = false };
    poller.workerCount++;

    MbpStatus ret = MbpLink_Open(&worker->link, conn->host, conn->port, station->unitId);
    MBP_ASSERT(ret);

    ret = MbpClock_WallMs(&wallMs);
    MBP_ASSERT_ERRNO(ret);
    ret = MbpHealth_Reset(&worker->health, wallMs);
    MBP_ASSERT(ret);
    ret = MbpStore_PutHealth(stationIndex, &worker->health);
    MBP_ASSERT(ret);

    ret = MbpClock_MonoMs(&nowMs);
    MBP_ASSERT_ERRNO(ret);
    for (uint16_t i = 0u; i < station->pointCount; i++) {
        worker->dueMs[i] = nowMs;
    }

    ret = CreateThread(worker);
    MBP_ASSERT(ret);
    return ret;
}

static MbpStatus CreateThread(Worker *worker) {
    pthread_attr_t attr;
    MbpStatus ret = OsStatus(pthread_attr_init(&attr));
    MBP_ASSERT_ERRNO(ret);

    ret = CreateThreadWith(worker, &attr);
    (void) pthread_attr_destroy(&attr);
    return ret;
}

static MbpStatus CreateThreadWith(Worker *worker, pthread_attr_t *attr) {
    int osRet = pthread_attr_setstacksize(attr, MBP_THREAD_STACK_BYTES);
    errno = osRet;
    MbpStatus ret = (osRet == 0) ? MbpStatusOk : MbpStatusThread;
    MBP_ASSERT_ERRNO(ret);

    osRet = pthread_create(&worker->thread, attr, WorkerMain, worker);
    errno = osRet;
    ret = (osRet == 0) ? MbpStatusOk : MbpStatusThread;
    MBP_ASSERT_ERRNO(ret);
    worker->isStarted = true;
    return ret;
}

static MbpStatus RequestStop(void) {
    MbpStatus ret = OsStatus(pthread_mutex_lock(&poller.mutex));
    MBP_ASSERT_ERRNO(ret);

    poller.isStopRequested = true;
    const MbpStatus broadcastRet = OsStatus(pthread_cond_broadcast(&poller.stopCond));
    ret = OsStatus(pthread_mutex_unlock(&poller.mutex));
    MBP_ASSERT_ERRNO(broadcastRet);
    MBP_ASSERT_ERRNO(ret);
    return ret;
}

// Ends with MbpStatusStopped on a stop, with any other status on a failure the worker cannot go on from.
static MbpStatus WorkerLoop(Worker *worker) {
    MbpStatus ret = MbpStatusOk;
    while (ret == MbpStatusOk) {
        ret = WorkerStep(worker);
    }
    return ret;
}

static MbpStatus WorkerStep(Worker *worker) {
    uint64_t nowMs = 0u;
    uint64_t waitMs = 0u;
    uint16_t pointIndex = 0u;
    if (!worker->link.isConnected) {
        return ConnectLink(worker);
    }
    MbpStatus ret = MbpClock_MonoMs(&nowMs);
    MBP_ASSERT_SILENT(ret);

    ret = MbpSched_PickNext(worker->dueMs, worker->station->pointCount, nowMs, &pointIndex, &waitMs);
    MBP_ASSERT_SILENT(ret);
    if (waitMs > 0u) {
        return WaitUntil(nowMs + waitMs);
    }

    ret = ReadPoint(worker, pointIndex);
    if (ret == MbpStatusStopped) {
        return ret;
    }
    return FinishRead(worker, pointIndex, ret);
}

// A failed connect waits the backoff before the next one.
static MbpStatus ConnectLink(Worker *worker) {
    uint64_t wallMs = 0u;
    uint64_t nowMs = 0u;
    MbpStatus ret = CheckStop();
    MBP_ASSERT_SILENT(ret);

    const MbpStatus result = MbpLink_Connect(&worker->link, worker->conn->connectTimeoutMs);
    ret = MbpClock_WallMs(&wallMs);
    MBP_ASSERT_SILENT(ret);
    const MbpHealthState before = worker->health.state;
    ret = MbpHealth_OnConnect(&worker->health, result, worker->conn->connectRetries, wallMs);
    MBP_ASSERT_SILENT(ret);
    LogTransition(worker, before, result);

    ret = MbpStore_PutHealth(worker->stationIndex, &worker->health);
    MBP_ASSERT_SILENT(ret);
    if (result == MbpStatusOk) {
        return ret;
    }
    ret = MbpClock_MonoMs(&nowMs);
    MBP_ASSERT_SILENT(ret);
    return WaitUntil(nowMs + worker->health.waitMs);
}

// Returns what MbpHealth_OnRead classifies: Ok, a point fault, a link fault, or MbpStatusStopped.
static MbpStatus ReadPoint(Worker *worker, uint16_t pointIndex) {
    const MbpCfgPoint *point = &worker->station->points[pointIndex];
    uint16_t regs[MBP_MAX_REGS] = { 0u };
    double value = 0.0;
    uint64_t wallMs = 0u;
    MbpStatus ret = ReadAttempts(worker, pointIndex, regs);
    MBP_ASSERT_SILENT(ret);

    ret = MbpClock_WallMs(&wallMs);
    MBP_ASSERT_SILENT(ret);
    const MbpStatus decodeRet = MbpDec_ToValue(regs, point->regCount, point->type, point->order, point->scale, &value);
    if (decodeRet == MbpStatusOk) {
        ret = MbpStore_PutValue(worker->stationIndex, pointIndex, value, wallMs);
    } else {
        ret = MbpStore_PutPointError(worker->stationIndex, pointIndex, decodeRet, wallMs);
    }
    MBP_ASSERT_SILENT(ret);
    return decodeRet;
}

// Up to 1 + request_retries attempts, the stop flag checked before each. A point fault is stored here, where it is
// classified; the link faults go back to the loop.
static MbpStatus ReadAttempts(Worker *worker, uint16_t pointIndex, uint16_t *regs) {
    const MbpCfgConn *conn = worker->conn;
    const MbpCfgPoint *point = &worker->station->points[pointIndex];
    MbpHealthStep step = MbpHealthStepRetry;
    MbpStatus result = MbpStatusLinkIo;
    uint64_t wallMs = 0u;
    for (uint8_t attempt = 0u; attempt <= conn->requestRetries; attempt++) {
        MbpStatus ret = CheckStop();
        MBP_ASSERT_SILENT(ret);
        if (step == MbpHealthStepReconnect) {
            ret = Reconnect(worker);
            MBP_ASSERT_SILENT(ret);
        }

        result = MbpLink_ReadRegs(&worker->link, point->funcCode, point->startAddr, point->regCount, regs, conn->requestTimeoutMs);
        ret = MbpHealth_NextStep(result, attempt, conn->requestRetries, &step);
        MBP_ASSERT_SILENT(ret);
        if ((step == MbpHealthStepDone) || (step == MbpHealthStepExhausted)) {
            return result;
        }
        if (step == MbpHealthStepPointFault) {
            ret = MbpClock_WallMs(&wallMs);
            MBP_ASSERT_SILENT(ret);
            ret = MbpStore_PutPointError(worker->stationIndex, pointIndex, result, wallMs);
            MBP_ASSERT_SILENT(ret);
            return result;
        }
    }
    return result;
}

// A retry after a timeout, bad data or an I/O error goes over a fresh connection, so a late reply on the old
// socket is never read as the retry's reply.
static MbpStatus Reconnect(Worker *worker) {
    uint64_t wallMs = 0u;
    MbpStatus ret = MbpLink_Disconnect(&worker->link);
    MBP_ASSERT_SILENT(ret);

    ret = MbpLink_Connect(&worker->link, worker->conn->connectTimeoutMs);
    MBP_ASSERT_SILENT(ret);

    ret = MbpClock_WallMs(&wallMs);
    MBP_ASSERT_SILENT(ret);
    return MbpHealth_OnConnect(&worker->health, MbpStatusOk, worker->conn->connectRetries, wallMs);
}

// A read that failed at link level leaves the station in error: the link is dropped and the worker backs off.
static MbpStatus FinishRead(Worker *worker, uint16_t pointIndex, MbpStatus result) {
    uint64_t wallMs = 0u;
    uint64_t nowMs = 0u;
    MbpStatus ret = MbpClock_WallMs(&wallMs);
    MBP_ASSERT_SILENT(ret);
    const MbpHealthState before = worker->health.state;
    ret = MbpHealth_OnRead(&worker->health, result, wallMs);
    MBP_ASSERT_SILENT(ret);
    LogTransition(worker, before, result);

    ret = MbpClock_MonoMs(&nowMs);
    MBP_ASSERT_SILENT(ret);
    ret = MbpSched_Advance(&worker->dueMs[pointIndex], worker->station->points[pointIndex].cycleMs, nowMs);
    MBP_ASSERT_SILENT(ret);
    if (worker->health.state != MbpHealthStateError) {
        return MbpStore_PutHealth(worker->stationIndex, &worker->health);
    }

    ret = DropLink(worker);
    MBP_ASSERT_SILENT(ret);
    return WaitUntil(nowMs + worker->health.waitMs);
}

static MbpStatus DropLink(Worker *worker) {
    MbpStatus ret = MbpLink_Disconnect(&worker->link);
    MBP_ASSERT_SILENT(ret);
    return MbpStore_PutLinkLoss(worker->stationIndex, &worker->health);
}

static MbpStatus MarkFailed(Worker *worker, MbpStatus cause) {
    uint64_t wallMs = 0u;
    MbpStatus ret = MbpClock_WallMs(&wallMs);
    MBP_ASSERT_SILENT(ret);
    const MbpHealthState before = worker->health.state;
    ret = MbpHealth_OnRead(&worker->health, cause, wallMs);
    MBP_ASSERT_SILENT(ret);
    LogTransition(worker, before, cause);
    return DropLink(worker);
}

static MbpStatus CheckStop(void) {
    MbpStatus ret = OsStatus(pthread_mutex_lock(&poller.mutex));
    MBP_ASSERT_SILENT(ret);
    const bool isStopping = poller.isStopRequested;
    ret = OsStatus(pthread_mutex_unlock(&poller.mutex));
    MBP_ASSERT_SILENT(ret);
    return isStopping ? MbpStatusStopped : MbpStatusOk;
}

static MbpStatus WaitUntil(uint64_t deadlineMs) {
    MbpStatus ret = OsStatus(pthread_mutex_lock(&poller.mutex));
    MBP_ASSERT_SILENT(ret);
    const MbpStatus waitRet = WaitLocked(deadlineMs);
    ret = OsStatus(pthread_mutex_unlock(&poller.mutex));
    MBP_ASSERT_SILENT(ret);
    return waitRet;
}

// Called with the mutex held. The stop flag is the predicate, read under the mutex, so a stop requested while the
// worker was inside libmodbus is seen here. A wait error other than ETIMEDOUT is a failure, never a timeout.
static MbpStatus WaitLocked(uint64_t deadlineMs) {
    uint64_t nowMs = 0u;
    MbpStatus ret = MbpClock_MonoMs(&nowMs);
    while ((ret == MbpStatusOk) && !poller.isStopRequested && ((int64_t) (deadlineMs - nowMs) > 0)) {
        const struct timespec deadline = { .tv_sec = (time_t) (deadlineMs / MS_PER_SEC), .tv_nsec = (long) ((deadlineMs % MS_PER_SEC) * NS_PER_MS) };
        const int waitRet = pthread_cond_timedwait(&poller.stopCond, &poller.mutex, &deadline);
        ret = ((waitRet == 0) || (waitRet == ETIMEDOUT)) ? MbpClock_MonoMs(&nowMs) : OsStatus(waitRet);
    }
    if ((ret == MbpStatusOk) && poller.isStopRequested) {
        ret = MbpStatusStopped;
    }
    return ret;
}

static void LogTransition(const Worker *worker, MbpHealthState before, MbpStatus cause) {
    const MbpHealthState after = worker->health.state;
    const char *beforeName = "";
    const char *afterName = "";
    if (after == before) {
        return;
    }
    (void) MbpHealth_StateName(before, &beforeName);
    (void) MbpHealth_StateName(after, &afterName);
    if (after == MbpHealthStateOnline) {
        MBP_LOG_INFO("station %s: %s -> %s (%s)", worker->station->id, beforeName, afterName, Mbp_StatusStr(cause));
    } else {
        MBP_LOG_WARN("station %s: %s -> %s (%s)", worker->station->id, beforeName, afterName, Mbp_StatusStr(cause));
    }
}

static void *WorkerMain(void *arg) {
    Worker *worker = arg;
    const MbpStatus ret = WorkerLoop(worker);
    if (ret != MbpStatusStopped) {
        MBP_LOG_ERR("station %s: worker ended: %s", worker->station->id, Mbp_StatusStr(ret));
        if (MarkFailed(worker, ret) != MbpStatusOk) {
            MBP_LOG_ERR("station %s: the failure could not be published", worker->station->id);
        }
    }
    return NULL;
}

MbpStatus MbpPoll_Start(const MbpCfg *cfg) {
    if (cfg == NULL) {
        return MbpStatusParam;
    }
    MbpStatus ret = CheckStackSize();
    MBP_ASSERT(ret);

    ret = InitStopCond();
    MBP_ASSERT(ret);

    for (uint16_t i = 0u; i < cfg->stationCount; i++) {
        if (cfg->conns[cfg->stations[i].connIndex].isEnabled) {
            ret = StartWorker(cfg, i);
            MBP_ASSERT(ret);
        }
    }
    return ret;
}

// Safe when never started, partly started or already stopped: joins only started threads, closes only opened links.
MbpStatus MbpPoll_Stop(void) {
    MbpStatus firstRet = MbpStatusOk;
    if (poller.isStopCondInit) {
        MBP_ASSERT_NON_FATAL(RequestStop(), firstRet);
    }
    for (uint16_t i = 0u; i < poller.workerCount; i++) {
        Worker *worker = &poller.workers[i];
        if (worker->isStarted) {
            MBP_ASSERT_NON_FATAL(OsStatus(pthread_join(worker->thread, NULL)), firstRet);
            worker->isStarted = false;
        }
        MBP_ASSERT_NON_FATAL(MbpLink_Close(&worker->link), firstRet);
    }
    poller.workerCount = 0u;
    if (poller.isStopCondInit) {
        MBP_ASSERT_NON_FATAL(OsStatus(pthread_cond_destroy(&poller.stopCond)), firstRet);
        poller.isStopCondInit = false;
    }
    return firstRet;
}
