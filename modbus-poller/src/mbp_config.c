#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <cjson/cJSON.h>

#include "mbp_config.h"
#include "mbp_config_field.h"
#include "mbp_decode.h"
#include "mbp_defs.h"

#define NO_INDEX INT32_C(-1)
#define PATH_TEXT_BYTES 160u
#define VALUE_EXCERPT_BYTES 40u
#define PORT_MIN 1u
#define PORT_MAX 65535u
#define REQUEST_TIMEOUT_MIN_MS 10u
#define REQUEST_TIMEOUT_MAX_MS 60000u
#define CONNECT_TIMEOUT_MIN_MS 100u
#define CONNECT_TIMEOUT_MAX_MS 60000u
#define RETRIES_MAX 10u
#define UNIT_ID_MIN 1u
#define UNIT_ID_SERIAL_MAX 247u
#define UNIT_ID_TCP 255u                    // the Modbus TCP "no serial unit" address
#define FUNC_CODE_MIN 3u                    // read holding registers
#define FUNC_CODE_MAX 4u                    // read input registers
#define ADDR_MAX 65535u
#define REG_SIZE_MAX 65535u
#define CYCLE_MIN_MS 100u
#define CYCLE_MAX_MS 86400000u

typedef struct {
    const char *name;
    int32_t value;
} Choice;

// Where the load stands, for the one error line: an index is NO_INDEX outside its array, field NULL for an entry.
typedef struct {
    MbpCfg *cfg;
    int32_t connIndex;
    int32_t stationIndex;
    int32_t pointIndex;
    bool isInMeta;
    const char *field;
    char value[VALUE_EXCERPT_BYTES];
} Loader;

static MbpStatus ReadFile(const char *path, char **text, size_t *textLen);
static MbpStatus ReadStream(FILE *file, char **text, size_t *textLen);
static MbpStatus LoadSteps(Loader *loader, const cJSON *root);
static MbpStatus LoadConn(Loader *loader, const cJSON *connJson);
static MbpStatus LoadConnFields(Loader *loader, const cJSON *connJson, MbpCfgConn *conn);
static MbpStatus LoadStation(Loader *loader, const cJSON *stationJson, uint16_t connIndex);
static MbpStatus LoadPoint(Loader *loader, const cJSON *pointJson, MbpCfgStation *station);
static MbpStatus LoadMeta(Loader *loader, const cJSON *pointJson, MbpCfgPoint *point);
static MbpStatus GetString(Loader *loader, const cJSON *object, const char *key, const char **text);
static MbpStatus GetArray(Loader *loader, const cJSON *object, const char *key, uint32_t maxCount, const cJSON **array);
static MbpStatus ReadUint(Loader *loader, const cJSON *object, const char *key, uint32_t min, uint32_t max, uint32_t *value);
static MbpStatus ReadBool(Loader *loader, const cJSON *object, const char *key, bool *value);
static MbpStatus ReadScale(Loader *loader, const cJSON *object, double *scale);
static MbpStatus ReadText(Loader *loader, const cJSON *object, const char *key, size_t minLen, char *dest, size_t destSize);
static MbpStatus ReadChoice(Loader *loader, const cJSON *object, const char *key, const Choice *choices, size_t choiceCount, int32_t *value);
static bool HasKey(const cJSON *object, const char *key);
static void MoveTo(Loader *loader, const char *field);
static void FormatPath(const Loader *loader, char *path, size_t pathSize);

static const Choice dataTypes[] = {
    { .name = "int16", .value = MbpDecTypeInt16 },
    { .name = "uint16", .value = MbpDecTypeUint16 },
    { .name = "int32", .value = MbpDecTypeInt32 },
    { .name = "uint32", .value = MbpDecTypeUint32 },
    { .name = "float32", .value = MbpDecTypeFloat32 },
};

static const Choice byteOrders[] = {
    { .name = "ABCD", .value = MbpDecOrderAbcd },
    { .name = "BADC", .value = MbpDecOrderBadc },
    { .name = "CDAB", .value = MbpDecOrderCdab },
    { .name = "DCBA", .value = MbpDecOrderDcba },
};

MBP_CT_ASSERT(MBP_MAX_CONNS <= UINT16_MAX, conn_count_fits_u16);
MBP_CT_ASSERT(MBP_MAX_STATIONS <= UINT16_MAX, station_count_fits_u16);
MBP_CT_ASSERT(MBP_MAX_POINTS <= UINT16_MAX, point_count_fits_u16);
MBP_CT_ASSERT(RETRIES_MAX <= UINT8_MAX, retries_fit_u8);
MBP_CT_ASSERT(UNIT_ID_TCP <= UINT8_MAX, unit_id_fits_u8);

static MbpStatus ReadFile(const char *path, char **text, size_t *textLen) {
    FILE *file = fopen(path, "rb");
    MbpStatus ret = (file != NULL) ? MbpStatusOk : MbpStatusCfgOpen;
    MBP_ASSERT_ERRNO(ret);

    ret = ReadStream(file, text, textLen);
    if ((fclose(file) != 0) && (ret == MbpStatusOk)) {
        ret = MbpStatusCfgRead;
    }
    return ret;
}

static MbpStatus ReadStream(FILE *file, char **text, size_t *textLen) {
    struct stat info;
    MbpStatus ret = (fstat(fileno(file), &info) == 0) ? MbpStatusOk : MbpStatusCfgRead;
    MBP_ASSERT_ERRNO(ret);

    ret = S_ISREG(info.st_mode) ? MbpStatusOk : MbpStatusCfgRead;
    MBP_ASSERT(ret);

    const size_t size = (size_t) info.st_size;
    char *buffer = malloc(size + 1u);
    ret = (buffer != NULL) ? MbpStatusOk : MbpStatusOs;
    MBP_ASSERT_ERRNO(ret);

    if (fread(buffer, 1u, size, file) != size) {
        free(buffer);
        return MbpStatusCfgRead;
    }
    buffer[size] = '\0';
    *text = buffer;
    *textLen = size;
    return ret;
}

static MbpStatus LoadSteps(Loader *loader, const cJSON *root) {
    const cJSON *conns = NULL;
    const cJSON *connJson = NULL;
    MoveTo(loader, NULL);
    MbpStatus ret = cJSON_IsObject(root) ? MbpStatusOk : MbpStatusCfgType;
    MBP_ASSERT(ret);

    ret = GetArray(loader, root, "modbus_tcp", MBP_MAX_CONNS, &conns);
    MBP_ASSERT(ret);

    loader->connIndex = 0;
    cJSON_ArrayForEach(connJson, conns) {
        ret = LoadConn(loader, connJson);
        MBP_ASSERT(ret);
        loader->connIndex++;
    }
    loader->connIndex = NO_INDEX;
    return ret;
}

static MbpStatus LoadConn(Loader *loader, const cJSON *connJson) {
    MbpCfg *cfg = loader->cfg;
    const uint16_t connIndex = cfg->connCount;
    const cJSON *stations = NULL;
    const cJSON *stationJson = NULL;
    MoveTo(loader, NULL);
    MbpStatus ret = cJSON_IsObject(connJson) ? MbpStatusOk : MbpStatusCfgType;
    MBP_ASSERT(ret);

    ret = LoadConnFields(loader, connJson, &cfg->conns[connIndex]);
    MBP_ASSERT(ret);
    cfg->connCount++;

    ret = GetArray(loader, connJson, "stations", MBP_MAX_STATIONS, &stations);
    MBP_ASSERT(ret);

    loader->stationIndex = 0;
    cJSON_ArrayForEach(stationJson, stations) {
        ret = LoadStation(loader, stationJson, connIndex);
        MBP_ASSERT(ret);
        loader->stationIndex++;
    }
    loader->stationIndex = NO_INDEX;
    return ret;
}

static MbpStatus LoadConnFields(Loader *loader, const cJSON *connJson, MbpCfgConn *conn) {
    const char *connType = NULL;
    uint32_t number = 0u;
    MbpStatus ret = ReadText(loader, connJson, "name", 1u, conn->name, sizeof(conn->name));
    MBP_ASSERT(ret);

    ret = GetString(loader, connJson, "conn_type", &connType);
    MBP_ASSERT(ret);
    ret = (strcmp(connType, "tcp") == 0) ? MbpStatusOk : MbpStatusCfgValue;
    MBP_ASSERT(ret);

    ret = ReadText(loader, connJson, "tcp_host", 1u, conn->host, sizeof(conn->host));
    MBP_ASSERT(ret);
    ret = MbpCfgField_CheckIpv4(conn->host);
    MBP_ASSERT(ret);

    ret = ReadUint(loader, connJson, "tcp_port", PORT_MIN, PORT_MAX, &number);
    MBP_ASSERT(ret);
    conn->port = (uint16_t) number;

    ret = ReadUint(loader, connJson, "request_timeout", REQUEST_TIMEOUT_MIN_MS, REQUEST_TIMEOUT_MAX_MS, &conn->requestTimeoutMs);
    MBP_ASSERT(ret);

    ret = ReadUint(loader, connJson, "request_retries", 0u, RETRIES_MAX, &number);
    MBP_ASSERT(ret);
    conn->requestRetries = (uint8_t) number;

    ret = ReadUint(loader, connJson, "connect_timeout", CONNECT_TIMEOUT_MIN_MS, CONNECT_TIMEOUT_MAX_MS, &conn->connectTimeoutMs);
    MBP_ASSERT(ret);

    ret = ReadUint(loader, connJson, "connect_retries", 0u, RETRIES_MAX, &number);
    MBP_ASSERT(ret);
    conn->connectRetries = (uint8_t) number;

    ret = ReadBool(loader, connJson, "enable", &conn->isEnabled);
    MBP_ASSERT(ret);
    return ret;
}

static MbpStatus LoadStation(Loader *loader, const cJSON *stationJson, uint16_t connIndex) {
    MbpCfg *cfg = loader->cfg;
    MbpCfgStation *station = &cfg->stations[cfg->stationCount];
    const cJSON *points = NULL;
    const cJSON *pointJson = NULL;
    uint32_t number = 0u;
    uint16_t existing = 0u;
    MoveTo(loader, NULL);
    MbpStatus ret = cJSON_IsObject(stationJson) ? MbpStatusOk : MbpStatusCfgType;
    MBP_ASSERT(ret);
    ret = (cfg->stationCount < MBP_MAX_STATIONS) ? MbpStatusOk : MbpStatusCfgLimit;
    MBP_ASSERT(ret);
    station->connIndex = connIndex;

    ret = ReadUint(loader, stationJson, "station_addr", UNIT_ID_MIN, UNIT_ID_TCP, &number);
    MBP_ASSERT(ret);
    ret = ((number <= UNIT_ID_SERIAL_MAX) || (number == UNIT_ID_TCP)) ? MbpStatusOk : MbpStatusCfgRange;
    MBP_ASSERT(ret);
    station->unitId = (uint8_t) number;

    ret = ReadText(loader, stationJson, "id", 1u, station->id, sizeof(station->id));
    MBP_ASSERT(ret);
    ret = (MbpCfg_FindStation(cfg, station->id, &existing) == MbpStatusOk) ? MbpStatusCfgDuplicate : MbpStatusOk;
    MBP_ASSERT(ret);

    ret = ReadText(loader, stationJson, "name", 1u, station->name, sizeof(station->name));
    MBP_ASSERT(ret);

    ret = GetArray(loader, stationJson, "datapoints", MBP_MAX_POINTS, &points);
    MBP_ASSERT(ret);

    loader->pointIndex = 0;
    cJSON_ArrayForEach(pointJson, points) {
        ret = LoadPoint(loader, pointJson, station);
        MBP_ASSERT(ret);
        loader->pointIndex++;
    }
    loader->pointIndex = NO_INDEX;
    cfg->stationCount++;
    return ret;
}

static MbpStatus LoadPoint(Loader *loader, const cJSON *pointJson, MbpCfgStation *station) {
    MbpCfgPoint *point = &station->points[station->pointCount];
    uint32_t number = 0u;
    int32_t choice = 0;
    uint16_t existing = 0u;
    MoveTo(loader, NULL);
    MbpStatus ret = cJSON_IsObject(pointJson) ? MbpStatusOk : MbpStatusCfgType;
    MBP_ASSERT(ret);

    ret = ReadText(loader, pointJson, "id", 1u, point->id, sizeof(point->id));
    MBP_ASSERT(ret);
    ret = (MbpCfg_FindPoint(station, point->id, &existing) == MbpStatusOk) ? MbpStatusCfgDuplicate : MbpStatusOk;
    MBP_ASSERT(ret);

    ret = ReadUint(loader, pointJson, "func_code", FUNC_CODE_MIN, FUNC_CODE_MAX, &number);
    MBP_ASSERT(ret);
    point->funcCode = (uint8_t) number;

    ret = ReadChoice(loader, pointJson, "data_type", dataTypes, sizeof(dataTypes) / sizeof(dataTypes[0]), &choice);
    MBP_ASSERT(ret);
    point->type = (MbpDecType) choice;

    ret = ReadChoice(loader, pointJson, "byte_order", byteOrders, sizeof(byteOrders) / sizeof(byteOrders[0]), &choice);
    MBP_ASSERT(ret);
    point->order = (MbpDecOrder) choice;

    ret = MbpDec_RegCount(point->type, &point->regCount);
    MBP_ASSERT(ret);
    ret = ReadUint(loader, pointJson, "reg_size", 0u, REG_SIZE_MAX, &number);
    MBP_ASSERT(ret);
    ret = (number == point->regCount) ? MbpStatusOk : MbpStatusCfgRegSize;
    MBP_ASSERT(ret);

    ret = ReadUint(loader, pointJson, "start_addr", 0u, ADDR_MAX, &number);
    MBP_ASSERT(ret);
    ret = ((number + point->regCount - 1u) <= ADDR_MAX) ? MbpStatusOk : MbpStatusCfgRange;
    MBP_ASSERT(ret);
    point->startAddr = (uint16_t) number;

    ret = ReadUint(loader, pointJson, "acq_cycle", CYCLE_MIN_MS, CYCLE_MAX_MS, &point->cycleMs);
    MBP_ASSERT(ret);

    ret = LoadMeta(loader, pointJson, point);
    MBP_ASSERT(ret);
    station->pointCount++;
    return ret;
}

static MbpStatus LoadMeta(Loader *loader, const cJSON *pointJson, MbpCfgPoint *point) {
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(pointJson, "meta");
    MbpStatus ret = MbpStatusOk;
    point->scale = 1.0;
    point->descr[0] = '\0';
    point->units[0] = '\0';
    MoveTo(loader, "meta");
    if (meta == NULL) {
        return ret;
    }
    ret = cJSON_IsObject(meta) ? MbpStatusOk : MbpStatusCfgType;
    MBP_ASSERT(ret);

    loader->isInMeta = true;
    if (HasKey(meta, "scale")) {
        ret = ReadScale(loader, meta, &point->scale);
        MBP_ASSERT(ret);
    }
    if (HasKey(meta, "descr")) {
        ret = ReadText(loader, meta, "descr", 0u, point->descr, sizeof(point->descr));
        MBP_ASSERT(ret);
    }
    if (HasKey(meta, "units")) {
        ret = ReadText(loader, meta, "units", 0u, point->units, sizeof(point->units));
        MBP_ASSERT(ret);
    }
    loader->isInMeta = false;
    return ret;
}

// Keeps a printable excerpt of every string read, so the failing step needs no extra bookkeeping.
static MbpStatus GetString(Loader *loader, const cJSON *object, const char *key, const char **text) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    MoveTo(loader, key);
    if (item == NULL) {
        return MbpStatusCfgMissing;
    }
    if (!cJSON_IsString(item) || (item->valuestring == NULL)) {
        return MbpStatusCfgType;
    }
    size_t len = 0u;
    while ((len < (VALUE_EXCERPT_BYTES - 1u)) && (item->valuestring[len] != '\0')) {
        const unsigned char byte = (unsigned char) item->valuestring[len];
        loader->value[len] = ((byte < 0x20u) || (byte >= 0x7Fu)) ? '?' : (char) byte;
        len++;
    }
    loader->value[len] = '\0';
    *text = item->valuestring;
    return MbpStatusOk;
}

static MbpStatus GetArray(Loader *loader, const cJSON *object, const char *key, uint32_t maxCount, const cJSON **array) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    MoveTo(loader, key);
    if (item == NULL) {
        return MbpStatusCfgMissing;
    }
    if (!cJSON_IsArray(item)) {
        return MbpStatusCfgType;
    }
    const int count = cJSON_GetArraySize(item);
    if ((count < 1) || ((uint32_t) count > maxCount)) {
        return MbpStatusCfgLimit;
    }
    *array = item;
    return MbpStatusOk;
}

static MbpStatus ReadUint(Loader *loader, const cJSON *object, const char *key, uint32_t min, uint32_t max, uint32_t *value) {
    const char *text = NULL;
    MbpStatus ret = GetString(loader, object, key, &text);
    MBP_ASSERT(ret);

    ret = MbpCfgField_ParseUint(text, min, max, value);
    MBP_ASSERT(ret);
    return ret;
}

static MbpStatus ReadBool(Loader *loader, const cJSON *object, const char *key, bool *value) {
    const char *text = NULL;
    MbpStatus ret = GetString(loader, object, key, &text);
    MBP_ASSERT(ret);

    ret = MbpCfgField_ParseBool(text, value);
    MBP_ASSERT(ret);
    return ret;
}

static MbpStatus ReadScale(Loader *loader, const cJSON *object, double *scale) {
    const char *text = NULL;
    MbpStatus ret = GetString(loader, object, "scale", &text);
    MBP_ASSERT(ret);

    ret = MbpCfgField_ParseScale(text, scale);
    MBP_ASSERT(ret);
    return ret;
}

static MbpStatus ReadText(Loader *loader, const cJSON *object, const char *key, size_t minLen, char *dest, size_t destSize) {
    const char *text = NULL;
    MbpStatus ret = GetString(loader, object, key, &text);
    MBP_ASSERT(ret);

    ret = MbpCfgField_CheckText(text, minLen, destSize - 1u);
    MBP_ASSERT(ret);
    (void) memcpy(dest, text, strlen(text) + 1u);
    return ret;
}

static MbpStatus ReadChoice(Loader *loader, const cJSON *object, const char *key, const Choice *choices, size_t choiceCount, int32_t *value) {
    const char *text = NULL;
    MbpStatus ret = GetString(loader, object, key, &text);
    MBP_ASSERT(ret);

    size_t index = 0u;
    while ((index < choiceCount) && (strcmp(choices[index].name, text) != 0)) {
        index++;
    }
    ret = (index < choiceCount) ? MbpStatusOk : MbpStatusCfgValue;
    MBP_ASSERT(ret);
    *value = choices[index].value;
    return ret;
}

static bool HasKey(const cJSON *object, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(object, key) != NULL;
}

static void MoveTo(Loader *loader, const char *field) {
    loader->field = field;
    loader->value[0] = '\0';
}

static void FormatPath(const Loader *loader, char *path, size_t pathSize) {
    const char *separator = (loader->field != NULL) ? "." : "";
    const char *field = (loader->field != NULL) ? loader->field : "";
    const char *meta = loader->isInMeta ? ".meta" : "";
    if (loader->connIndex == NO_INDEX) {
        (void) snprintf(path, pathSize, "%s", (loader->field != NULL) ? loader->field : "(top level)");
    } else if (loader->stationIndex == NO_INDEX) {
        (void) snprintf(path, pathSize, "modbus_tcp[%" PRId32 "]%s%s", loader->connIndex, separator, field);
    } else if (loader->pointIndex == NO_INDEX) {
        (void) snprintf(path, pathSize, "modbus_tcp[%" PRId32 "].stations[%" PRId32 "]%s%s", loader->connIndex, loader->stationIndex, separator, field);
    } else {
        (void) snprintf(path, pathSize, "modbus_tcp[%" PRId32 "].stations[%" PRId32 "].datapoints[%" PRId32 "]%s%s%s", loader->connIndex, loader->stationIndex, loader->pointIndex, meta, separator, field);
    }
}

MbpStatus MbpCfg_Load(const char *path, MbpCfg *cfg) {
    if ((path == NULL) || (cfg == NULL)) {
        return MbpStatusParam;
    }
    Loader loader = { .cfg = cfg, .connIndex = NO_INDEX, .stationIndex = NO_INDEX, .pointIndex = NO_INDEX, .isInMeta = false, .field = NULL, .value = "" };
    char *text = NULL;
    size_t textLen = 0u;
    const char *parseEnd = NULL;
    (void) memset(cfg, 0, sizeof(*cfg));

    MbpStatus ret = ReadFile(path, &text, &textLen);
    if (ret != MbpStatusOk) {
        MBP_LOG_ERR("config: %s: %s", path, Mbp_StatusStr(ret));
        return ret;
    }
    cJSON *root = cJSON_ParseWithLengthOpts(text, textLen, &parseEnd, false);
    const size_t errorOffset = (parseEnd != NULL) ? (size_t) (parseEnd - text) : 0u;
    free(text);
    if (root == NULL) {
        MBP_LOG_ERR("config: %s: %s at byte %zu", path, Mbp_StatusStr(MbpStatusCfgJson), errorOffset);
        return MbpStatusCfgJson;
    }

    ret = LoadSteps(&loader, root);
    cJSON_Delete(root);
    if (ret != MbpStatusOk) {
        char where[PATH_TEXT_BYTES];
        FormatPath(&loader, where, sizeof(where));
        if (loader.value[0] != '\0') {
            MBP_LOG_ERR("config: %s: %s (\"%s\")", where, Mbp_StatusStr(ret), loader.value);
        } else {
            MBP_LOG_ERR("config: %s: %s", where, Mbp_StatusStr(ret));
        }
    }
    return ret;
}

MbpStatus MbpCfg_FindStation(const MbpCfg *cfg, const char *deviceId, uint16_t *stationIndex) {
    if ((cfg == NULL) || (deviceId == NULL) || (stationIndex == NULL)) {
        return MbpStatusParam;
    }
    for (uint16_t i = 0u; i < cfg->stationCount; i++) {
        if (strcmp(cfg->stations[i].id, deviceId) == 0) {
            *stationIndex = i;
            return MbpStatusOk;
        }
    }
    return MbpStatusProtoUnknownDevice;
}

MbpStatus MbpCfg_FindPoint(const MbpCfgStation *station, const char *pointId, uint16_t *pointIndex) {
    if ((station == NULL) || (pointId == NULL) || (pointIndex == NULL)) {
        return MbpStatusParam;
    }
    for (uint16_t i = 0u; i < station->pointCount; i++) {
        if (strcmp(station->points[i].id, pointId) == 0) {
            *pointIndex = i;
            return MbpStatusOk;
        }
    }
    return MbpStatusProtoUnknownPoint;
}
