#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mbp_config.h"
#include "mbp_config_field.h"
#include "mbp_test_check.h"

#define PATH_BYTES 512u
#define REL_PATH_BYTES 128u
#define SCALE_TOL 1e-12

typedef struct {
    const char *file;
    MbpStatus expected;
} InvalidCase;

static MbpStatus LoadFile(const char *root, const char *relPath);
static void TestProvided(const char *root);
static void TestAllTypes(const char *root);
static void TestInvalid(const char *root);
static void TestUnreadable(const char *root);
static void TestFind(const char *root);
static void TestParseUint(void);
static void TestParseScale(void);
static void TestParseBool(void);
static void TestCheckIpv4(void);
static void TestCheckText(void);

static const InvalidCase invalidCases[] = {
    { .file = "data_type_float64.json", .expected = MbpStatusCfgValue },
    { .file = "byte_order_abdc.json", .expected = MbpStatusCfgValue },
    { .file = "func_code_5.json", .expected = MbpStatusCfgRange },
    { .file = "reg_size_float32_1.json", .expected = MbpStatusCfgRegSize },
    { .file = "tcp_port_abc.json", .expected = MbpStatusCfgValue },
    { .file = "tcp_port_number.json", .expected = MbpStatusCfgType },
    { .file = "duplicate_station_id.json", .expected = MbpStatusCfgDuplicate },
    { .file = "duplicate_point_id.json", .expected = MbpStatusCfgDuplicate },
    { .file = "tcp_host_missing.json", .expected = MbpStatusCfgMissing },
    { .file = "tcp_host_name.json", .expected = MbpStatusCfgValue },
    { .file = "enable_yes.json", .expected = MbpStatusCfgValue },
    { .file = "start_addr_70000.json", .expected = MbpStatusCfgRange },
    { .file = "start_addr_past_end.json", .expected = MbpStatusCfgRange },
    { .file = "start_addr_space.json", .expected = MbpStatusCfgValue },
    { .file = "start_addr_plus.json", .expected = MbpStatusCfgValue },
    { .file = "station_addr_250.json", .expected = MbpStatusCfgRange },
    { .file = "station_addr_0.json", .expected = MbpStatusCfgRange },
    { .file = "name_control_char.json", .expected = MbpStatusCfgValue },
    { .file = "scale_overflow.json", .expected = MbpStatusCfgRange },
    { .file = "truncated.json", .expected = MbpStatusCfgJson },
    { .file = "trailing_text.json", .expected = MbpStatusCfgJson },
};

static MbpCfg cfg;

static MbpStatus LoadFile(const char *root, const char *relPath) {
    char path[PATH_BYTES];
    (void) snprintf(path, sizeof(path), "%s/%s", root, relPath);
    return MbpCfg_Load(path, &cfg);
}

static void TestProvided(const char *root) {
    MBP_CHECK_STATUS(LoadFile(root, "config/modbus_config_tcp.json"), MbpStatusOk);
    MBP_CHECK(cfg.connCount == 1u);
    MBP_CHECK(cfg.stationCount == 2u);
    const MbpCfgConn *conn = &cfg.conns[0];
    MBP_CHECK(strcmp(conn->name, "Modbus TCP Client 1") == 0);
    MBP_CHECK(strcmp(conn->host, "192.168.200.1") == 0);
    MBP_CHECK(conn->port == 502u);
    MBP_CHECK(conn->requestTimeoutMs == 2000u);
    MBP_CHECK(conn->requestRetries == 3u);
    MBP_CHECK(conn->connectTimeoutMs == 5000u);
    MBP_CHECK(conn->connectRetries == 3u);
    MBP_CHECK(conn->isEnabled);
    for (uint16_t i = 0u; i < cfg.stationCount; i++) {
        MBP_CHECK(cfg.stations[i].pointCount == 13u);
        MBP_CHECK(cfg.stations[i].unitId == (uint8_t) (i + 1u));
        MBP_CHECK(cfg.stations[i].connIndex == 0u);
    }
    MBP_CHECK(strcmp(cfg.stations[0].id, "1") == 0);
    MBP_CHECK(strcmp(cfg.stations[1].name, "Electricity Meter Ser. EM1277") == 0);
    const MbpCfgPoint *power = &cfg.stations[0].points[6];
    MBP_CHECK(strcmp(power->id, "12") == 0);
    MBP_CHECK(strcmp(power->descr, "Total system power") == 0);
    MBP_CHECK(strcmp(power->units, "kW") == 0);
    MBP_CHECK(power->type == MbpDecTypeFloat32);
    MBP_CHECK(power->order == MbpDecOrderAbcd);
    MBP_CHECK(power->funcCode == 4u);
    MBP_CHECK(power->startAddr == 52u);
    MBP_CHECK(power->regCount == 2u);
    MBP_CHECK(power->cycleMs == 10000u);
    MBP_CHECK_NEAR(power->scale, 0.001, SCALE_TOL);
    MBP_CHECK(cfg.stations[1].points[12].cycleMs == 3600000u);
}

static void TestAllTypes(const char *root) {
    MBP_CHECK_STATUS(LoadFile(root, "tests/fixtures/all_types.json"), MbpStatusOk);
    MBP_CHECK(cfg.connCount == 2u);
    MBP_CHECK(cfg.stationCount == 3u);
    MBP_CHECK(cfg.stations[2].connIndex == 1u);
    for (uint16_t s = 0u; s < 2u; s++) {
        const MbpCfgStation *station = &cfg.stations[s];
        MBP_CHECK(station->pointCount == 40u);
        for (uint8_t funcCode = 3u; funcCode <= 4u; funcCode++) {
            for (int type = 0; type < (int) MbpDecTypeCount; type++) {
                for (int order = 0; order < (int) MbpDecOrderCount; order++) {
                    unsigned found = 0u;
                    for (uint16_t p = 0u; p < station->pointCount; p++) {
                        const MbpCfgPoint *point = &station->points[p];
                        if ((point->funcCode == funcCode) && ((int) point->type == type) && ((int) point->order == order)) {
                            found++;
                        }
                    }
                    MBP_CHECK(found == 1u);
                }
            }
        }
    }
    const MbpCfgPoint *noMeta = &cfg.stations[2].points[4];
    MBP_CHECK_NEAR(noMeta->scale, 1.0, SCALE_TOL);
    MBP_CHECK(noMeta->descr[0] == '\0');
    MBP_CHECK(noMeta->units[0] == '\0');
}

static void TestInvalid(const char *root) {
    char relPath[REL_PATH_BYTES];
    for (size_t i = 0u; i < (sizeof(invalidCases) / sizeof(invalidCases[0])); i++) {
        (void) snprintf(relPath, sizeof(relPath), "tests/fixtures/invalid/%s", invalidCases[i].file);
        MBP_CHECK_STATUS(LoadFile(root, relPath), invalidCases[i].expected);
    }
}

static void TestUnreadable(const char *root) {
    MBP_CHECK_STATUS(LoadFile(root, "config/no_such_file.json"), MbpStatusCfgOpen);
    MBP_CHECK_STATUS(LoadFile(root, "config"), MbpStatusCfgRead);
    MBP_CHECK_STATUS(MbpCfg_Load(NULL, &cfg), MbpStatusParam);
}

static void TestFind(const char *root) {
    uint16_t index = 99u;
    MBP_CHECK_STATUS(LoadFile(root, "tests/fixtures/all_types.json"), MbpStatusOk);
    MBP_CHECK_STATUS(MbpCfg_FindStation(&cfg, "3", &index), MbpStatusOk);
    MBP_CHECK(index == 2u);
    MBP_CHECK_STATUS(MbpCfg_FindStation(&cfg, "4", &index), MbpStatusProtoUnknownDevice);
    MBP_CHECK_STATUS(MbpCfg_FindPoint(&cfg.stations[0], "40", &index), MbpStatusOk);
    MBP_CHECK(index == 39u);
    MBP_CHECK_STATUS(MbpCfg_FindPoint(&cfg.stations[0], "41", &index), MbpStatusProtoUnknownPoint);
    MBP_CHECK_STATUS(MbpCfg_FindPoint(&cfg.stations[0], "", &index), MbpStatusProtoUnknownPoint);
}

static void TestParseUint(void) {
    uint32_t value = 0u;
    MBP_CHECK_STATUS(MbpCfgField_ParseUint("010", 0u, 100u, &value), MbpStatusOk);
    MBP_CHECK(value == 10u);
    MBP_CHECK_STATUS(MbpCfgField_ParseUint("4294967295", 0u, UINT32_MAX, &value), MbpStatusOk);
    MBP_CHECK(value == UINT32_MAX);
    MBP_CHECK_STATUS(MbpCfgField_ParseUint("4294967296", 0u, UINT32_MAX, &value), MbpStatusCfgRange);
    MBP_CHECK_STATUS(MbpCfgField_ParseUint("99999999999999999999999", 0u, UINT32_MAX, &value), MbpStatusCfgRange);
    MBP_CHECK_STATUS(MbpCfgField_ParseUint("101", 0u, 100u, &value), MbpStatusCfgRange);
    MBP_CHECK_STATUS(MbpCfgField_ParseUint("9", 10u, 100u, &value), MbpStatusCfgRange);
    MBP_CHECK_STATUS(MbpCfgField_ParseUint(" 10", 0u, 100u, &value), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_ParseUint("+10", 0u, 100u, &value), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_ParseUint("-1", 0u, 100u, &value), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_ParseUint("10 ", 0u, 100u, &value), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_ParseUint("0x10", 0u, 100u, &value), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_ParseUint("", 0u, 100u, &value), MbpStatusCfgValue);
}

static void TestParseScale(void) {
    double value = 0.0;
    MBP_CHECK_STATUS(MbpCfgField_ParseScale("0.001", &value), MbpStatusOk);
    MBP_CHECK_NEAR(value, 0.001, SCALE_TOL);
    MBP_CHECK_STATUS(MbpCfgField_ParseScale("-2.5e3", &value), MbpStatusOk);
    MBP_CHECK_NEAR(value, -2500.0, SCALE_TOL);
    MBP_CHECK_STATUS(MbpCfgField_ParseScale("1e999", &value), MbpStatusCfgRange);
    MBP_CHECK_STATUS(MbpCfgField_ParseScale("inf", &value), MbpStatusCfgRange);
    MBP_CHECK_STATUS(MbpCfgField_ParseScale("nan", &value), MbpStatusCfgRange);
    MBP_CHECK_STATUS(MbpCfgField_ParseScale(" 1", &value), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_ParseScale("1 ", &value), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_ParseScale("1x", &value), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_ParseScale("", &value), MbpStatusCfgValue);
}

static void TestParseBool(void) {
    bool value = false;
    MBP_CHECK_STATUS(MbpCfgField_ParseBool("true", &value), MbpStatusOk);
    MBP_CHECK(value);
    MBP_CHECK_STATUS(MbpCfgField_ParseBool("false", &value), MbpStatusOk);
    MBP_CHECK(!value);
    MBP_CHECK_STATUS(MbpCfgField_ParseBool("True", &value), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_ParseBool("1", &value), MbpStatusCfgValue);
}

static void TestCheckIpv4(void) {
    MBP_CHECK_STATUS(MbpCfgField_CheckIpv4("192.168.200.1"), MbpStatusOk);
    MBP_CHECK_STATUS(MbpCfgField_CheckIpv4("127.0.0.1"), MbpStatusOk);
    MBP_CHECK_STATUS(MbpCfgField_CheckIpv4("localhost"), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_CheckIpv4("::1"), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_CheckIpv4("1.2.3"), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_CheckIpv4("256.1.1.1"), MbpStatusCfgValue);
}

static void TestCheckText(void) {
    MBP_CHECK_STATUS(MbpCfgField_CheckText("Meter", 1u, 15u), MbpStatusOk);
    MBP_CHECK_STATUS(MbpCfgField_CheckText("\xC2\xB0" "C", 0u, 15u), MbpStatusOk);
    MBP_CHECK_STATUS(MbpCfgField_CheckText("\xE2\x82\xAC\xF0\x9F\x98\x80", 0u, 15u), MbpStatusOk);
    MBP_CHECK_STATUS(MbpCfgField_CheckText("", 0u, 15u), MbpStatusOk);
    MBP_CHECK_STATUS(MbpCfgField_CheckText("", 1u, 15u), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_CheckText("0123456789abcdef", 1u, 15u), MbpStatusCfgTooLong);
    MBP_CHECK_STATUS(MbpCfgField_CheckText("\xB0" "C", 0u, 15u), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_CheckText("ab\xC3", 0u, 15u), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_CheckText("a\x01", 0u, 15u), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_CheckText("a\x7F", 0u, 15u), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_CheckText("\xC0\xAF", 0u, 15u), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_CheckText("\xED\xA0\x80", 0u, 15u), MbpStatusCfgValue);
    MBP_CHECK_STATUS(MbpCfgField_CheckText("\xF4\x90\x80\x80", 0u, 15u), MbpStatusCfgValue);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        (void) fprintf(stderr, "usage: %s <source root>\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *root = argv[1];
    TestProvided(root);
    TestAllTypes(root);
    TestInvalid(root);
    TestUnreadable(root);
    TestFind(root);
    TestParseUint();
    TestParseScale();
    TestParseBool();
    TestCheckIpv4();
    TestCheckText();
    return MBP_TEST_RESULT();
}
