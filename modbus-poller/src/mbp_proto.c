#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "mbp_config.h"
#include "mbp_defs.h"
#include "mbp_health.h"
#include "mbp_proto.h"
#include "mbp_store.h"

#define ARENA_BYTES 24576u
#define ARENA_ALIGN _Alignof(max_align_t)
// cJSON recurses parse_value -> parse_object or parse_array once per nesting level. A level's frames, with
// parse_number's 64-byte buffer, stay well under 1 KB on x86_64 (an estimate from the 1.7.15 source, not measured).
#define CJSON_LEVEL_STACK_BYTES 1024u

typedef struct {
    const char *name;
    MbpProtoCmd cmd;
} CmdName;

typedef struct {
    MbpStatus status;
    const char *code;
} WireCode;

typedef struct {
    _Alignas(max_align_t) unsigned char bytes[ARENA_BYTES];
    size_t used;
    bool isExhausted;
} Arena;

static MbpStatus CheckLine(const char *line, size_t len);
static MbpStatus ReadRequest(const cJSON *root, MbpProtoRequest *request);
static MbpStatus ReadId(const cJSON *root, const char *key, MbpStatus tooLong, char *dest, size_t destSize);
static MbpStatus CheckBuf(const MbpProtoBuf *buf);
static MbpStatus Append(MbpProtoBuf *buf, const char *format, ...) __attribute__((format(printf, 2, 3)));
static MbpStatus AppendString(MbpProtoBuf *buf, const char *key, const char *text);
static MbpStatus AppendTime(MbpProtoBuf *buf, const char *key, uint64_t timeMs);
static MbpStatus AppendError(MbpProtoBuf *buf, const char *key, MbpStatus error);
static MbpStatus AppendDevice(MbpProtoBuf *buf, const MbpCfg *cfg, uint16_t stationIndex);
static MbpStatus AppendPoint(MbpProtoBuf *buf, const MbpCfgStation *station, const MbpStoreStation *copy, uint16_t pointIndex);
static const char *WireCodeOf(MbpStatus status);
static void *ArenaAlloc(size_t size);
static void ArenaFree(void *block);

// wire protocol v1: the command names; clients send these; additive only
static const CmdName cmdNames[] = {
    { .name = "list", .cmd = MbpProtoCmdList },
    { .name = "status", .cmd = MbpProtoCmdStatus },
    { .name = "values", .cmd = MbpProtoCmdValues },
    { .name = "value", .cmd = MbpProtoCmdValue },
};

// wire protocol v1: the error codes; clients store these; additive only. Any other status is "internal".
static const WireCode wireCodes[] = {
    { .status = MbpStatusProtoJson, .code = "bad_request" },
    { .status = MbpStatusProtoField, .code = "bad_request" },
    { .status = MbpStatusProtoDepth, .code = "bad_request" },
    { .status = MbpStatusProtoArena, .code = "bad_request" },
    { .status = MbpStatusProtoUnknownCmd, .code = "unknown_command" },
    { .status = MbpStatusProtoUnknownDevice, .code = "unknown_device" },
    { .status = MbpStatusProtoUnknownPoint, .code = "unknown_point" },
    { .status = MbpStatusIpcTooLong, .code = "too_long" },
    { .status = MbpStatusIpcBusy, .code = "busy" },
};

// wire protocol v1: the quality names; clients store these; additive only
static const char *const qualityNames[MbpStoreQualityCount] = {
    [MbpStoreQualityNone] = "none",
    [MbpStoreQualityGood] = "good",
    [MbpStoreQualityStale] = "stale",
    [MbpStoreQualityBad] = "bad",
};

static Arena arena;

// A line of MBP_PROTO_LINE_MAX bytes holds at most LINE_MAX / 2 + 1 items, each one aligned allocation, plus at
// most LINE_MAX bytes of keys and strings, each with its terminator.
MBP_CT_ASSERT(ARENA_BYTES >= ((((MBP_PROTO_LINE_MAX / 2u) + 1u) * (sizeof(cJSON) + ARENA_ALIGN)) + (2u * MBP_PROTO_LINE_MAX)), arena_holds_worst_request);
// 4 levels x 1 KB = 4 KB, inside the quarter of the IPC thread's 64 KB stack this assert reserves; poll, the client
// table walk and the reply writers use the rest.
MBP_CT_ASSERT((MBP_PROTO_MAX_DEPTH * CJSON_LEVEL_STACK_BYTES) <= (MBP_THREAD_STACK_BYTES / 4u), cjson_recursion_fits_ipc_stack);

static MbpStatus CheckLine(const char *line, size_t len) {
    uint32_t depth = 0u;
    bool isInString = false;
    bool isEscaped = false;
    if (len > MBP_PROTO_LINE_MAX) {
        return MbpStatusProtoArena;
    }
    if (memchr(line, '\0', len) != NULL) {
        return MbpStatusProtoJson;
    }
    for (size_t i = 0u; i < len; i++) {
        const char byte = line[i];
        if (isEscaped) {
            isEscaped = false;
            if ((byte == 'u') && ((i + 4u) < len) && (memcmp(&line[i + 1u], "0000", 4u) == 0)) {
                return MbpStatusProtoJson;
            }
        } else if (isInString) {
            isEscaped = (byte == '\\');
            isInString = (byte != '"');
        } else if (byte == '"') {
            isInString = true;
        } else if ((byte == '{') || (byte == '[')) {
            depth++;
            if (depth > MBP_PROTO_MAX_DEPTH) {
                return MbpStatusProtoDepth;
            }
        } else if (((byte == '}') || (byte == ']')) && (depth > 0u)) {
            depth--;
        }
    }
    return MbpStatusOk;
}

static MbpStatus ReadRequest(const cJSON *root, MbpProtoRequest *request) {
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsObject(root) || !cJSON_IsString(cmd) || (cmd->valuestring == NULL)) {
        return MbpStatusProtoField;
    }
    size_t index = 0u;
    while ((index < (sizeof(cmdNames) / sizeof(cmdNames[0]))) && (strcmp(cmdNames[index].name, cmd->valuestring) != 0)) {
        index++;
    }
    MbpStatus ret = (index < (sizeof(cmdNames) / sizeof(cmdNames[0]))) ? MbpStatusOk : MbpStatusProtoUnknownCmd;
    MBP_ASSERT_SILENT(ret);

    request->cmd = cmdNames[index].cmd;
    request->device[0] = '\0';
    request->point[0] = '\0';
    if (request->cmd != MbpProtoCmdList) {
        ret = ReadId(root, "device", MbpStatusProtoUnknownDevice, request->device, sizeof(request->device));
        MBP_ASSERT_SILENT(ret);
    }
    if (request->cmd == MbpProtoCmdValue) {
        ret = ReadId(root, "point", MbpStatusProtoUnknownPoint, request->point, sizeof(request->point));
        MBP_ASSERT_SILENT(ret);
    }
    return ret;
}

static MbpStatus ReadId(const cJSON *root, const char *key, MbpStatus tooLong, char *dest, size_t destSize) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || (item->valuestring == NULL)) {
        return MbpStatusProtoField;
    }
    const size_t len = strlen(item->valuestring);
    if (len >= destSize) {
        return tooLong;
    }
    (void) memcpy(dest, item->valuestring, len + 1u);
    return MbpStatusOk;
}

static MbpStatus CheckBuf(const MbpProtoBuf *buf) {
    return ((buf != NULL) && (buf->text != NULL) && (buf->size > 0u)) ? MbpStatusOk : MbpStatusParam;
}

static MbpStatus Append(MbpProtoBuf *buf, const char *format, ...) {
    va_list args;
    const size_t room = buf->size - buf->len;
    va_start(args, format);
    const int written = vsnprintf(&buf->text[buf->len], room, format, args);
    va_end(args);
    if ((written < 0) || ((size_t) written >= room)) {
        return MbpStatusProtoOverflow;
    }
    buf->len += (size_t) written;
    return MbpStatusOk;
}

static MbpStatus AppendString(MbpProtoBuf *buf, const char *key, const char *text) {
    MbpStatus ret = Append(buf, "%s\"", key);
    MBP_ASSERT_SILENT(ret);
    for (const char *next = text; *next != '\0'; next++) {
        const bool needsEscape = (*next == '"') || (*next == '\\');
        const size_t need = needsEscape ? MBP_PROTO_ESCAPE_GROWTH : 1u;
        if ((buf->len + need) >= buf->size) {
            return MbpStatusProtoOverflow;
        }
        if (needsEscape) {
            buf->text[buf->len] = '\\';
            buf->len++;
        }
        buf->text[buf->len] = *next;
        buf->len++;
    }
    buf->text[buf->len] = '\0';
    return Append(buf, "\"");
}

static MbpStatus AppendTime(MbpProtoBuf *buf, const char *key, uint64_t timeMs) {
    if (timeMs == 0u) {
        return Append(buf, "%snull", key);
    }
    return Append(buf, "%s%" PRIu64, key, timeMs);
}

static MbpStatus AppendError(MbpProtoBuf *buf, const char *key, MbpStatus error) {
    if (error == MbpStatusOk) {
        return Append(buf, "%snull", key);
    }
    return AppendString(buf, key, Mbp_StatusStr(error));
}

static MbpStatus AppendDevice(MbpProtoBuf *buf, const MbpCfg *cfg, uint16_t stationIndex) {
    const MbpCfgStation *station = &cfg->stations[stationIndex];
    const MbpCfgConn *conn = &cfg->conns[station->connIndex];
    MbpStatus ret = AppendString(buf, "{\"id\":", station->id);
    MBP_ASSERT_SILENT(ret);
    ret = AppendString(buf, ",\"name\":", station->name);
    MBP_ASSERT_SILENT(ret);
    ret = AppendString(buf, ",\"connection\":", conn->name);
    MBP_ASSERT_SILENT(ret);
    ret = AppendString(buf, ",\"host\":", conn->host);
    MBP_ASSERT_SILENT(ret);
    return Append(buf, ",\"port\":%u,\"unit\":%u,\"enabled\":%s,\"points\":%u}", (unsigned) conn->port, (unsigned) station->unitId, conn->isEnabled ? "true" : "false", (unsigned) station->pointCount);
}

static MbpStatus AppendPoint(MbpProtoBuf *buf, const MbpCfgStation *station, const MbpStoreStation *copy, uint16_t pointIndex) {
    const MbpCfgPoint *cfgPoint = &station->points[pointIndex];
    const MbpStorePoint *point = &copy->points[pointIndex];
    MbpStatus ret = ((unsigned) point->quality < (unsigned) MbpStoreQualityCount) ? MbpStatusOk : MbpStatusParam;
    MBP_ASSERT_SILENT(ret);
    ret = AppendString(buf, "{\"device\":", station->id);
    MBP_ASSERT_SILENT(ret);
    ret = AppendString(buf, ",\"id\":", cfgPoint->id);
    MBP_ASSERT_SILENT(ret);
    ret = AppendString(buf, ",\"descr\":", cfgPoint->descr);
    MBP_ASSERT_SILENT(ret);
    ret = AppendString(buf, ",\"units\":", cfgPoint->units);
    MBP_ASSERT_SILENT(ret);
    ret = point->hasValue ? Append(buf, ",\"value\":%.10g", point->value) : Append(buf, ",\"value\":null");
    MBP_ASSERT_SILENT(ret);
    ret = AppendTime(buf, ",\"ts\":", point->tsMs);
    MBP_ASSERT_SILENT(ret);
    ret = Append(buf, ",\"quality\":\"%s\"", qualityNames[point->quality]);
    MBP_ASSERT_SILENT(ret);
    ret = AppendError(buf, ",\"error\":", point->error);
    MBP_ASSERT_SILENT(ret);
    return Append(buf, "}");
}

static const char *WireCodeOf(MbpStatus status) {
    for (size_t i = 0u; i < (sizeof(wireCodes) / sizeof(wireCodes[0])); i++) {
        if (wireCodes[i].status == status) {
            return wireCodes[i].code;
        }
    }
    return "internal";
}

static void *ArenaAlloc(size_t size) {
    const size_t aligned = ((size + ARENA_ALIGN - 1u) / ARENA_ALIGN) * ARENA_ALIGN;
    if ((aligned < size) || (aligned > (ARENA_BYTES - arena.used))) {
        arena.isExhausted = true;
        return NULL;
    }
    void *block = &arena.bytes[arena.used];
    arena.used += aligned;
    return block;
}

static void ArenaFree(void *block) {
    (void) block;
}

// Called after MbpCfg_Load has freed its tree: from here on cJSON allocates only from the arena.
MbpStatus MbpProto_Init(void) {
    cJSON_Hooks hooks = { .malloc_fn = ArenaAlloc, .free_fn = ArenaFree };
    arena.used = 0u;
    arena.isExhausted = false;
    cJSON_InitHooks(&hooks);
    return MbpStatusOk;
}

MbpStatus MbpProto_Deinit(void) {
    cJSON_InitHooks(NULL);
    return MbpStatusOk;
}

MbpStatus MbpProto_ParseRequest(const char *line, size_t len, MbpProtoRequest *request) {
    arena.used = 0u;
    arena.isExhausted = false;
    if ((line == NULL) || (request == NULL)) {
        return MbpStatusParam;
    }
    MbpStatus ret = CheckLine(line, len);
    MBP_ASSERT_SILENT(ret);

    cJSON *root = cJSON_ParseWithLength(line, len);
    if (root == NULL) {
        return arena.isExhausted ? MbpStatusProtoArena : MbpStatusProtoJson;
    }
    ret = ReadRequest(root, request);
    cJSON_Delete(root);
    return ret;
}

MbpStatus MbpProto_WriteList(const MbpCfg *cfg, MbpProtoBuf *buf) {
    MbpStatus ret = (cfg != NULL) ? CheckBuf(buf) : MbpStatusParam;
    MBP_ASSERT_SILENT(ret);
    buf->len = 0u;
    ret = Append(buf, "{\"ok\":true,\"result\":[");
    MBP_ASSERT_SILENT(ret);
    for (uint16_t i = 0u; i < cfg->stationCount; i++) {
        if (i > 0u) {
            ret = Append(buf, ",");
            MBP_ASSERT_SILENT(ret);
        }
        ret = AppendDevice(buf, cfg, i);
        MBP_ASSERT_SILENT(ret);
    }
    return Append(buf, "]}\n");
}

MbpStatus MbpProto_WriteStatus(const MbpCfgStation *station, const MbpStoreStation *copy, MbpProtoBuf *buf) {
    const char *stateName = NULL;
    MbpStatus ret = ((station != NULL) && (copy != NULL)) ? CheckBuf(buf) : MbpStatusParam;
    MBP_ASSERT_SILENT(ret);
    const MbpHealth *health = &copy->health;
    ret = MbpHealth_StateName(health->state, &stateName);
    MBP_ASSERT_SILENT(ret);
    buf->len = 0u;
    ret = AppendString(buf, "{\"ok\":true,\"result\":{\"id\":", station->id);
    MBP_ASSERT_SILENT(ret);
    ret = Append(buf, ",\"state\":\"%s\"", stateName);
    MBP_ASSERT_SILENT(ret);
    ret = AppendTime(buf, ",\"since\":", health->sinceMs);
    MBP_ASSERT_SILENT(ret);
    ret = AppendTime(buf, ",\"last_ok\":", health->lastOkMs);
    MBP_ASSERT_SILENT(ret);
    ret = AppendError(buf, ",\"last_error\":", health->lastError);
    MBP_ASSERT_SILENT(ret);
    return Append(buf, ",\"reads_ok\":%" PRIu64 ",\"reads_failed\":%" PRIu64 ",\"reconnects\":%" PRIu64 "}}\n", health->readsOk, health->readsFailed, health->reconnects);
}

MbpStatus MbpProto_WriteValues(const MbpCfgStation *station, const MbpStoreStation *copy, MbpProtoBuf *buf) {
    MbpStatus ret = ((station != NULL) && (copy != NULL) && (copy->pointCount == station->pointCount)) ? CheckBuf(buf) : MbpStatusParam;
    MBP_ASSERT_SILENT(ret);
    buf->len = 0u;
    ret = AppendString(buf, "{\"ok\":true,\"result\":{\"id\":", station->id);
    MBP_ASSERT_SILENT(ret);
    ret = Append(buf, ",\"points\":[");
    MBP_ASSERT_SILENT(ret);
    for (uint16_t i = 0u; i < copy->pointCount; i++) {
        if (i > 0u) {
            ret = Append(buf, ",");
            MBP_ASSERT_SILENT(ret);
        }
        ret = AppendPoint(buf, station, copy, i);
        MBP_ASSERT_SILENT(ret);
    }
    return Append(buf, "]}}\n");
}

MbpStatus MbpProto_WriteValue(const MbpCfgStation *station, const MbpStoreStation *copy, uint16_t pointIndex, MbpProtoBuf *buf) {
    MbpStatus ret = ((station != NULL) && (copy != NULL) && (pointIndex < copy->pointCount) && (pointIndex < station->pointCount)) ? CheckBuf(buf) : MbpStatusParam;
    MBP_ASSERT_SILENT(ret);
    buf->len = 0u;
    ret = Append(buf, "{\"ok\":true,\"result\":");
    MBP_ASSERT_SILENT(ret);
    ret = AppendPoint(buf, station, copy, pointIndex);
    MBP_ASSERT_SILENT(ret);
    return Append(buf, "}\n");
}

MbpStatus MbpProto_WriteError(MbpStatus status, MbpProtoBuf *buf) {
    MbpStatus ret = CheckBuf(buf);
    MBP_ASSERT_SILENT(ret);
    buf->len = 0u;
    ret = Append(buf, "{\"ok\":false,\"error\":{\"code\":\"%s\"", WireCodeOf(status));
    MBP_ASSERT_SILENT(ret);
    ret = AppendString(buf, ",\"message\":", Mbp_StatusStr(status));
    MBP_ASSERT_SILENT(ret);
    return Append(buf, "}}\n");
}
