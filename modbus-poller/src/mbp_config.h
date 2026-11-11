// Internal to modbus-poller. External users include mbp.h.
#ifndef MBP_CONFIG_H
#define MBP_CONFIG_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#include "mbp.h"
#include "mbp_decode.h"
#include "mbp_defs.h"

#define MBP_CFG_NAME_MAX 63u
#define MBP_CFG_ID_MAX 15u
#define MBP_CFG_DESCR_MAX 127u
#define MBP_CFG_UNITS_MAX 15u

typedef struct {
    char id[MBP_CFG_ID_MAX + 1u];
    char descr[MBP_CFG_DESCR_MAX + 1u];
    char units[MBP_CFG_UNITS_MAX + 1u];
    MbpDecType type;
    MbpDecOrder order;
    uint8_t funcCode;
    uint16_t startAddr;
    uint16_t regCount;
    uint32_t cycleMs;
    double scale;
} MbpCfgPoint;

typedef struct {
    char id[MBP_CFG_ID_MAX + 1u];
    char name[MBP_CFG_NAME_MAX + 1u];
    uint8_t unitId;
    uint16_t connIndex;
    uint16_t pointCount;
    MbpCfgPoint points[MBP_MAX_POINTS];
} MbpCfgStation;

typedef struct {
    char name[MBP_CFG_NAME_MAX + 1u];
    char host[INET_ADDRSTRLEN];
    uint16_t port;
    uint32_t requestTimeoutMs;
    uint8_t requestRetries;                 // extra attempts after the first
    uint32_t connectTimeoutMs;
    uint8_t connectRetries;                 // failed connects tolerated before the station reports offline
    bool isEnabled;
} MbpCfgConn;

// Stations of every connection in file order; a station names its connection by index.
typedef struct {
    uint16_t connCount;
    uint16_t stationCount;
    MbpCfgConn conns[MBP_MAX_CONNS];
    MbpCfgStation stations[MBP_MAX_STATIONS];
} MbpCfg;

MbpStatus MbpCfg_Load(const char *path, MbpCfg *cfg);
MbpStatus MbpCfg_FindStation(const MbpCfg *cfg, const char *deviceId, uint16_t *stationIndex);
MbpStatus MbpCfg_FindPoint(const MbpCfgStation *station, const char *pointId, uint16_t *pointIndex);

#endif // MBP_CONFIG_H
