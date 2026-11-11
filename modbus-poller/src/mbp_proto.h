// Internal to modbus-poller. External users include mbp.h.
#ifndef MBP_PROTO_H
#define MBP_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "mbp.h"
#include "mbp_config.h"
#include "mbp_store.h"

#define MBP_PROTO_LINE_MAX 512u             // request bytes before the newline
#define MBP_PROTO_MAX_DEPTH 4u
#define MBP_PROTO_ESCAPE_GROWTH 2u          // only " and \ are escaped: config strings hold no control characters

// wire protocol v1: the command names; clients send these; additive only
typedef enum {
    MbpProtoCmdList = 0,
    MbpProtoCmdStatus,
    MbpProtoCmdValues,
    MbpProtoCmdValue,
    MbpProtoCmdCount,
} MbpProtoCmd;

// The ids are copies, so nothing points into the parse arena after MbpProto_ParseRequest returns.
typedef struct {
    MbpProtoCmd cmd;
    char device[MBP_CFG_ID_MAX + 1u];
    char point[MBP_CFG_ID_MAX + 1u];
} MbpProtoRequest;

// Caller-owned reply text; every writer replaces the content with one whole line, newline included.
typedef struct {
    char *text;
    size_t size;
    size_t len;
} MbpProtoBuf;

MbpStatus MbpProto_Init(void);
MbpStatus MbpProto_Deinit(void);
MbpStatus MbpProto_ParseRequest(const char *line, size_t len, MbpProtoRequest *request);
MbpStatus MbpProto_WriteList(const MbpCfg *cfg, MbpProtoBuf *buf);
MbpStatus MbpProto_WriteStatus(const MbpCfgStation *station, const MbpStoreStation *copy, MbpProtoBuf *buf);
MbpStatus MbpProto_WriteValues(const MbpCfgStation *station, const MbpStoreStation *copy, MbpProtoBuf *buf);
MbpStatus MbpProto_WriteValue(const MbpCfgStation *station, const MbpStoreStation *copy, uint16_t pointIndex, MbpProtoBuf *buf);
MbpStatus MbpProto_WriteError(MbpStatus status, MbpProtoBuf *buf);

#endif // MBP_PROTO_H
