#include <stddef.h>
#include <stdint.h>

#include "mbp_config.h"
#include "mbp_defs.h"
#include "mbp_proto.h"
#include "mbp_serve.h"
#include "mbp_store.h"

typedef MbpStatus (*ServeFn)(const MbpCfg *cfg, const MbpProtoRequest *request, MbpProtoBuf *reply);

static MbpStatus ServeList(const MbpCfg *cfg, const MbpProtoRequest *request, MbpProtoBuf *reply);
static MbpStatus ServeStatus(const MbpCfg *cfg, const MbpProtoRequest *request, MbpProtoBuf *reply);
static MbpStatus ServeValues(const MbpCfg *cfg, const MbpProtoRequest *request, MbpProtoBuf *reply);
static MbpStatus ServeValue(const MbpCfg *cfg, const MbpProtoRequest *request, MbpProtoBuf *reply);
static MbpStatus CopyDevice(const MbpCfg *cfg, const char *deviceId, uint16_t *stationIndex, MbpStoreStation *copy);

static const ServeFn serveFns[MbpProtoCmdCount] = {
    [MbpProtoCmdList] = ServeList,
    [MbpProtoCmdStatus] = ServeStatus,
    [MbpProtoCmdValues] = ServeValues,
    [MbpProtoCmdValue] = ServeValue,
};

static MbpStatus ServeList(const MbpCfg *cfg, const MbpProtoRequest *request, MbpProtoBuf *reply) {
    (void) request;
    return MbpProto_WriteList(cfg, reply);
}

static MbpStatus ServeStatus(const MbpCfg *cfg, const MbpProtoRequest *request, MbpProtoBuf *reply) {
    MbpStoreStation copy;
    uint16_t stationIndex = 0u;
    MbpStatus ret = CopyDevice(cfg, request->device, &stationIndex, &copy);
    MBP_ASSERT_SILENT(ret);
    return MbpProto_WriteStatus(&cfg->stations[stationIndex], &copy, reply);
}

static MbpStatus ServeValues(const MbpCfg *cfg, const MbpProtoRequest *request, MbpProtoBuf *reply) {
    MbpStoreStation copy;
    uint16_t stationIndex = 0u;
    MbpStatus ret = CopyDevice(cfg, request->device, &stationIndex, &copy);
    MBP_ASSERT_SILENT(ret);
    return MbpProto_WriteValues(&cfg->stations[stationIndex], &copy, reply);
}

static MbpStatus ServeValue(const MbpCfg *cfg, const MbpProtoRequest *request, MbpProtoBuf *reply) {
    MbpStoreStation copy;
    uint16_t stationIndex = 0u;
    uint16_t pointIndex = 0u;
    MbpStatus ret = MbpCfg_FindStation(cfg, request->device, &stationIndex);
    MBP_ASSERT_SILENT(ret);
    ret = MbpCfg_FindPoint(&cfg->stations[stationIndex], request->point, &pointIndex);
    MBP_ASSERT_SILENT(ret);
    ret = MbpStore_CopyStation(stationIndex, &copy);
    MBP_ASSERT_SILENT(ret);
    return MbpProto_WriteValue(&cfg->stations[stationIndex], &copy, pointIndex, reply);
}

// The store lock is held only for the slot copy; the reply is formatted from the copy.
static MbpStatus CopyDevice(const MbpCfg *cfg, const char *deviceId, uint16_t *stationIndex, MbpStoreStation *copy) {
    MbpStatus ret = MbpCfg_FindStation(cfg, deviceId, stationIndex);
    MBP_ASSERT_SILENT(ret);
    return MbpStore_CopyStation(*stationIndex, copy);
}

// On a failure the reply holds nothing usable: the caller writes the error reply.
MbpStatus MbpServe_Line(const MbpCfg *cfg, const char *line, size_t len, MbpProtoBuf *reply) {
    MbpProtoRequest request;
    MbpStatus ret = ((cfg != NULL) && (line != NULL) && (reply != NULL)) ? MbpStatusOk : MbpStatusParam;
    MBP_ASSERT_SILENT(ret);

    ret = MbpProto_ParseRequest(line, len, &request);
    MBP_ASSERT_SILENT(ret);

    ret = ((unsigned) request.cmd < (unsigned) MbpProtoCmdCount) ? MbpStatusOk : MbpStatusParam;
    MBP_ASSERT_SILENT(ret);
    return serveFns[request.cmd](cfg, &request, reply);
}
