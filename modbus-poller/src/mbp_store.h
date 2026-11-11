// Internal to modbus-poller. External users include mbp.h.
#ifndef MBP_STORE_H
#define MBP_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "mbp.h"
#include "mbp_config.h"
#include "mbp_defs.h"
#include "mbp_health.h"

// wire protocol v1: the names of these qualities; clients store these; additive only
typedef enum {
    MbpStoreQualityNone = 0,                // never read
    MbpStoreQualityGood,
    MbpStoreQualityStale,                   // the latest read failed; value and tsMs are the last good reading
    MbpStoreQualityBad,                     // the latest read got a reply that decodes to no finite value
    MbpStoreQualityCount,
} MbpStoreQuality;

typedef struct {
    double value;
    uint64_t tsMs;                          // wall-clock epoch ms of the read behind value, 0 = never
    MbpStoreQuality quality;
    MbpStatus error;                        // the point's latest failure, Ok while good
    bool hasValue;
} MbpStorePoint;

typedef struct {
    MbpHealth health;
    uint16_t pointCount;
    MbpStorePoint points[MBP_MAX_POINTS];
} MbpStoreStation;

MbpStatus MbpStore_Init(const MbpCfg *cfg);
MbpStatus MbpStore_Deinit(void);
MbpStatus MbpStore_PutValue(uint16_t stationIndex, uint16_t pointIndex, double value, uint64_t tsMs);
MbpStatus MbpStore_PutPointError(uint16_t stationIndex, uint16_t pointIndex, MbpStatus error, uint64_t tsMs);
MbpStatus MbpStore_PutHealth(uint16_t stationIndex, const MbpHealth *health);
MbpStatus MbpStore_PutLinkLoss(uint16_t stationIndex, const MbpHealth *health);
MbpStatus MbpStore_CopyStation(uint16_t stationIndex, MbpStoreStation *copy);

#endif // MBP_STORE_H
