#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "mbp_clock.h"
#include "mbp_defs.h"
#include "mbp_health.h"
#include "mbp_store.h"

// storeMutex guards every slot of store.stations. Each worker thread writes only its own station's slot; the IPC
// thread copies slots out. Init and Deinit run on the main thread while no worker or IPC thread runs.
typedef struct {
    bool isInit;
    uint16_t stationCount;
    MbpStoreStation stations[MBP_MAX_STATIONS];
} Store;

static MbpStatus CheckStation(uint16_t stationIndex);
static MbpStatus CheckPoint(uint16_t stationIndex, uint16_t pointIndex);
static MbpStatus Lock(void);
static MbpStatus Unlock(void);
static void WriteValue(MbpStorePoint *point, double value, uint64_t tsMs);
static void WritePointError(MbpStorePoint *point, MbpStatus error, uint64_t tsMs);
static void WriteLinkLoss(MbpStoreStation *station, const MbpHealth *health);

static pthread_mutex_t storeMutex = PTHREAD_MUTEX_INITIALIZER;
static Store store;

MBP_CT_ASSERT(sizeof(MbpStoreStation) <= 2560u, slot_copy_stays_short);

static MbpStatus CheckStation(uint16_t stationIndex) {
    return (store.isInit && (stationIndex < store.stationCount)) ? MbpStatusOk : MbpStatusParam;
}

static MbpStatus CheckPoint(uint16_t stationIndex, uint16_t pointIndex) {
    MbpStatus ret = CheckStation(stationIndex);
    MBP_ASSERT_SILENT(ret);
    return (pointIndex < store.stations[stationIndex].pointCount) ? MbpStatusOk : MbpStatusParam;
}

static MbpStatus Lock(void) {
    const int lockRet = pthread_mutex_lock(&storeMutex);
    if (lockRet != 0) {
        errno = lockRet;
        return MbpStatusOs;
    }
    return MbpStatusOk;
}

static MbpStatus Unlock(void) {
    const int unlockRet = pthread_mutex_unlock(&storeMutex);
    if (unlockRet != 0) {
        errno = unlockRet;
        return MbpStatusOs;
    }
    return MbpStatusOk;
}

static void WriteValue(MbpStorePoint *point, double value, uint64_t tsMs) {
    point->value = value;
    point->tsMs = tsMs;
    point->quality = MbpStoreQualityGood;
    point->error = MbpStatusOk;
    point->hasValue = true;
}

// A reply that decodes to no finite value is a reading of its own (bad, no value); any other failure keeps the last
// reading and marks it stale.
static void WritePointError(MbpStorePoint *point, MbpStatus error, uint64_t tsMs) {
    point->error = error;
    if (error == MbpStatusDecNotFinite) {
        point->quality = MbpStoreQualityBad;
        point->tsMs = tsMs;
        point->hasValue = false;
    } else {
        point->quality = MbpStoreQualityStale;
    }
}

static void WriteLinkLoss(MbpStoreStation *station, const MbpHealth *health) {
    station->health = *health;
    for (uint16_t i = 0u; i < station->pointCount; i++) {
        station->points[i].quality = MbpStoreQualityStale;
        station->points[i].error = health->lastError;
    }
}

MbpStatus MbpStore_Init(const MbpCfg *cfg) {
    uint64_t nowMs = 0u;
    if ((cfg == NULL) || (cfg->stationCount > MBP_MAX_STATIONS)) {
        return MbpStatusParam;
    }
    MbpStatus ret = MbpClock_WallMs(&nowMs);
    MBP_ASSERT_ERRNO(ret);

    (void) memset(&store, 0, sizeof(store));
    for (uint16_t i = 0u; i < cfg->stationCount; i++) {
        const MbpCfgStation *station = &cfg->stations[i];
        MbpStoreStation *slot = &store.stations[i];
        slot->pointCount = station->pointCount;
        if (cfg->conns[station->connIndex].isEnabled) {
            ret = MbpHealth_Reset(&slot->health, nowMs);
        } else {
            ret = MbpHealth_Disable(&slot->health, nowMs);
        }
        MBP_ASSERT(ret);
    }
    store.stationCount = cfg->stationCount;
    store.isInit = true;
    return ret;
}

MbpStatus MbpStore_Deinit(void) {
    store.isInit = false;
    store.stationCount = 0u;
    return MbpStatusOk;
}

MbpStatus MbpStore_PutValue(uint16_t stationIndex, uint16_t pointIndex, double value, uint64_t tsMs) {
    MbpStatus ret = CheckPoint(stationIndex, pointIndex);
    MBP_ASSERT_SILENT(ret);
    ret = Lock();
    MBP_ASSERT_SILENT(ret);
    WriteValue(&store.stations[stationIndex].points[pointIndex], value, tsMs);
    return Unlock();
}

MbpStatus MbpStore_PutPointError(uint16_t stationIndex, uint16_t pointIndex, MbpStatus error, uint64_t tsMs) {
    MbpStatus ret = CheckPoint(stationIndex, pointIndex);
    MBP_ASSERT_SILENT(ret);
    ret = Lock();
    MBP_ASSERT_SILENT(ret);
    WritePointError(&store.stations[stationIndex].points[pointIndex], error, tsMs);
    return Unlock();
}

MbpStatus MbpStore_PutHealth(uint16_t stationIndex, const MbpHealth *health) {
    MbpStatus ret = (health != NULL) ? CheckStation(stationIndex) : MbpStatusParam;
    MBP_ASSERT_SILENT(ret);
    ret = Lock();
    MBP_ASSERT_SILENT(ret);
    store.stations[stationIndex].health = *health;
    return Unlock();
}

// The health and the stale marks go out in one lock section, so a reader never sees error beside good points.
MbpStatus MbpStore_PutLinkLoss(uint16_t stationIndex, const MbpHealth *health) {
    MbpStatus ret = (health != NULL) ? CheckStation(stationIndex) : MbpStatusParam;
    MBP_ASSERT_SILENT(ret);
    ret = Lock();
    MBP_ASSERT_SILENT(ret);
    WriteLinkLoss(&store.stations[stationIndex], health);
    return Unlock();
}

MbpStatus MbpStore_CopyStation(uint16_t stationIndex, MbpStoreStation *copy) {
    MbpStatus ret = (copy != NULL) ? CheckStation(stationIndex) : MbpStatusParam;
    MBP_ASSERT_SILENT(ret);
    ret = Lock();
    MBP_ASSERT_SILENT(ret);
    *copy = store.stations[stationIndex];
    return Unlock();
}
