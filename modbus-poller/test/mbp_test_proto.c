#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mbp_config.h"
#include "mbp_proto.h"
#include "mbp_store.h"
#include "mbp_test_check.h"

#define REPLY_TEST_BYTES 4096u
#define LONG_LINE_BYTES 600u

typedef struct {
    const char *line;
    MbpStatus expected;
} ParseCase;

static void CheckReply(const MbpProtoBuf *buf, const char *expected);
static void FillFixture(void);
static void TestParseCases(void);
static void TestParseIds(void);
static void TestParseLongLines(void);
static void TestWriters(void);
static void TestWriteErrors(void);
static void TestOverflow(void);

static const ParseCase parseCases[] = {
    { .line = "{\"cmd\":\"list\"}", .expected = MbpStatusOk },
    { .line = "{\"cmd\":\"status\",\"device\":\"1\"}", .expected = MbpStatusOk },
    { .line = "{\"cmd\":\"values\",\"device\":\"1\"}", .expected = MbpStatusOk },
    { .line = "{\"cmd\":\"value\",\"device\":\"1\",\"point\":\"2\"}", .expected = MbpStatusOk },
    { .line = "  {\"cmd\":\"list\"}\r", .expected = MbpStatusOk },
    { .line = "{\"cmd\":\"value\",\"device\":\"1\"}", .expected = MbpStatusProtoField },
    { .line = "{\"cmd\":\"status\",\"device\":1}", .expected = MbpStatusProtoField },
    { .line = "{\"cmd\":5}", .expected = MbpStatusProtoField },
    { .line = "{\"device\":\"1\"}", .expected = MbpStatusProtoField },
    { .line = "[\"list\"]", .expected = MbpStatusProtoField },
    { .line = "{\"cmd\":\"reboot\"}", .expected = MbpStatusProtoUnknownCmd },
    { .line = "not json", .expected = MbpStatusProtoJson },
    { .line = "{\"cmd\":\"list\"", .expected = MbpStatusProtoJson },
    { .line = "", .expected = MbpStatusProtoJson },
    { .line = "{\"cmd\":\"status\",\"device\":\"1\\u0000x\"}", .expected = MbpStatusProtoJson },
    { .line = "{\"cmd\":\"status\",\"device\":\"a\\\\u0000\"}", .expected = MbpStatusOk },
    { .line = "[[[[1]]]]", .expected = MbpStatusProtoField },
    { .line = "[[[[[1]]]]]", .expected = MbpStatusProtoDepth },
    { .line = "{\"cmd\":\"list\",\"p\":\"[[[[[[\"}", .expected = MbpStatusOk },
    { .line = "{\"cmd\":\"status\",\"device\":\"0123456789abcdef\"}", .expected = MbpStatusProtoUnknownDevice },
    { .line = "{\"cmd\":\"value\",\"device\":\"1\",\"point\":\"0123456789abcdef\"}", .expected = MbpStatusProtoUnknownPoint },
};

static MbpCfg cfg;
static MbpStoreStation copy;

static void CheckReply(const MbpProtoBuf *buf, const char *expected) {
    if ((buf->len != strlen(expected)) || (memcmp(buf->text, expected, buf->len) != 0)) {
        (void) fprintf(stderr, "reply mismatch\n  got:      %.*s\n  expected: %s\n", (int) buf->len, buf->text, expected);
        mbpTestFailures++;
    }
}

static void FillFixture(void) {
    MbpCfgConn *conn = &cfg.conns[0];
    MbpCfgStation *station = &cfg.stations[0];
    (void) memset(&cfg, 0, sizeof(cfg));
    (void) memset(&copy, 0, sizeof(copy));
    cfg.connCount = 1u;
    cfg.stationCount = 1u;
    (void) strcpy(conn->name, "Gateway");
    (void) strcpy(conn->host, "127.0.0.1");
    conn->port = 502u;
    conn->isEnabled = true;
    (void) strcpy(station->id, "7");
    (void) strcpy(station->name, "Meter \"7\"");
    station->unitId = 3u;
    station->pointCount = 2u;
    (void) strcpy(station->points[0].id, "1");
    (void) strcpy(station->points[0].descr, "Volts");
    (void) strcpy(station->points[0].units, "V");
    (void) strcpy(station->points[1].id, "2");
    (void) strcpy(station->points[1].descr, "a\"b\\c");
    copy.pointCount = 2u;
    copy.points[0] = (MbpStorePoint) { .value = 230.5, .tsMs = UINT64_C(1700000000000), .quality = MbpStoreQualityGood, .error = MbpStatusOk, .hasValue = true };
    copy.points[1] = (MbpStorePoint) { .value = 0.0, .tsMs = 0u, .quality = MbpStoreQualityStale, .error = MbpStatusLinkExcIllegalAddress, .hasValue = false };
    copy.health = (MbpHealth) { .state = MbpHealthStateOnline, .sinceMs = UINT64_C(1700000000001), .lastOkMs = UINT64_C(1700000000002), .lastError = MbpStatusLinkTimeout, .readsOk = 5u, .readsFailed = 2u, .reconnects = 1u };
}

static void TestParseCases(void) {
    for (size_t i = 0u; i < (sizeof(parseCases) / sizeof(parseCases[0])); i++) {
        MbpProtoRequest request;
        const MbpStatus ret = MbpProto_ParseRequest(parseCases[i].line, strlen(parseCases[i].line), &request);
        if (ret != parseCases[i].expected) {
            (void) fprintf(stderr, "parse %s: got \"%s\", expected \"%s\"\n", parseCases[i].line, Mbp_StatusStr(ret), Mbp_StatusStr(parseCases[i].expected));
            mbpTestFailures++;
        }
    }
}

static void TestParseIds(void) {
    MbpProtoRequest request;
    const char *value = "{\"cmd\":\"value\",\"device\":\"012345678901234\",\"point\":\"p\\\"q\"}";
    MBP_CHECK_STATUS(MbpProto_ParseRequest(value, strlen(value), &request), MbpStatusOk);
    MBP_CHECK(request.cmd == MbpProtoCmdValue);
    MBP_CHECK(strcmp(request.device, "012345678901234") == 0);
    MBP_CHECK(strcmp(request.point, "p\"q") == 0);
    const char withNul[] = "{\"cmd\":\"list\"}\0x";
    MBP_CHECK_STATUS(MbpProto_ParseRequest(withNul, sizeof(withNul) - 1u, &request), MbpStatusProtoJson);
    const char *list = "{\"cmd\":\"list\"}";
    MBP_CHECK_STATUS(MbpProto_ParseRequest(list, strlen(list), &request), MbpStatusOk);
    MBP_CHECK(request.cmd == MbpProtoCmdList);
    MBP_CHECK(request.device[0] == '\0');
}

// The worst shapes that fit one line: the arena must hold them, and a reset must make room for the next.
static void TestParseLongLines(void) {
    static char line[LONG_LINE_BYTES];
    MbpProtoRequest request;
    size_t len = (size_t) snprintf(line, sizeof(line), "{\"cmd\":\"list\",\"p\":[0");
    while ((len + 4u) < MBP_PROTO_LINE_MAX) {
        line[len] = ',';
        line[len + 1u] = '0';
        len += 2u;
    }
    line[len] = ']';
    line[len + 1u] = '}';
    len += 2u;
    for (unsigned i = 0u; i < 1000u; i++) {
        MBP_CHECK_STATUS(MbpProto_ParseRequest(line, len, &request), MbpStatusOk);
    }
    len = (size_t) snprintf(line, sizeof(line), "{\"cmd\":\"list\",\"p\":[\"\"");
    while ((len + 5u) < MBP_PROTO_LINE_MAX) {
        (void) memcpy(&line[len], ",\"\"", 3u);
        len += 3u;
    }
    line[len] = ']';
    line[len + 1u] = '}';
    len += 2u;
    MBP_CHECK_STATUS(MbpProto_ParseRequest(line, len, &request), MbpStatusOk);
    (void) memset(line, ' ', sizeof(line));
    MBP_CHECK_STATUS(MbpProto_ParseRequest(line, MBP_PROTO_LINE_MAX + 1u, &request), MbpStatusProtoArena);
}

static void TestWriters(void) {
    static char text[REPLY_TEST_BYTES];
    MbpProtoBuf buf = { .text = text, .size = sizeof(text), .len = 0u };
    FillFixture();
    MBP_CHECK_STATUS(MbpProto_WriteList(&cfg, &buf), MbpStatusOk);
    CheckReply(&buf, "{\"ok\":true,\"result\":[{\"id\":\"7\",\"name\":\"Meter \\\"7\\\"\",\"connection\":\"Gateway\",\"host\":\"127.0.0.1\",\"port\":502,\"unit\":3,\"enabled\":true,\"points\":2}]}\n");
    MBP_CHECK_STATUS(MbpProto_WriteStatus(&cfg.stations[0], &copy, &buf), MbpStatusOk);
    CheckReply(&buf, "{\"ok\":true,\"result\":{\"id\":\"7\",\"state\":\"online\",\"since\":1700000000001,\"last_ok\":1700000000002,\"last_error\":\"modbus response timeout\",\"reads_ok\":5,\"reads_failed\":2,\"reconnects\":1}}\n");
    MBP_CHECK_STATUS(MbpProto_WriteValues(&cfg.stations[0], &copy, &buf), MbpStatusOk);
    CheckReply(&buf, "{\"ok\":true,\"result\":{\"id\":\"7\",\"points\":["
                     "{\"device\":\"7\",\"id\":\"1\",\"descr\":\"Volts\",\"units\":\"V\",\"value\":230.5,\"ts\":1700000000000,\"quality\":\"good\",\"error\":null},"
                     "{\"device\":\"7\",\"id\":\"2\",\"descr\":\"a\\\"b\\\\c\",\"units\":\"\",\"value\":null,\"ts\":null,\"quality\":\"stale\",\"error\":\"modbus exception 02 illegal data address\"}]}}\n");
    MBP_CHECK_STATUS(MbpProto_WriteValue(&cfg.stations[0], &copy, 0u, &buf), MbpStatusOk);
    CheckReply(&buf, "{\"ok\":true,\"result\":{\"device\":\"7\",\"id\":\"1\",\"descr\":\"Volts\",\"units\":\"V\",\"value\":230.5,\"ts\":1700000000000,\"quality\":\"good\",\"error\":null}}\n");
    MBP_CHECK_STATUS(MbpProto_WriteValue(&cfg.stations[0], &copy, 2u, &buf), MbpStatusParam);
}

static void TestWriteErrors(void) {
    static char text[REPLY_TEST_BYTES];
    MbpProtoBuf buf = { .text = text, .size = sizeof(text), .len = 0u };
    MBP_CHECK_STATUS(MbpProto_WriteError(MbpStatusProtoUnknownDevice, &buf), MbpStatusOk);
    CheckReply(&buf, "{\"ok\":false,\"error\":{\"code\":\"unknown_device\",\"message\":\"unknown device\"}}\n");
    MBP_CHECK_STATUS(MbpProto_WriteError(MbpStatusProtoArena, &buf), MbpStatusOk);
    CheckReply(&buf, "{\"ok\":false,\"error\":{\"code\":\"bad_request\",\"message\":\"request too large to parse\"}}\n");
    MBP_CHECK_STATUS(MbpProto_WriteError(MbpStatusIpcTooLong, &buf), MbpStatusOk);
    CheckReply(&buf, "{\"ok\":false,\"error\":{\"code\":\"too_long\",\"message\":\"request line too long\"}}\n");
    MBP_CHECK_STATUS(MbpProto_WriteError(MbpStatusIpcBusy, &buf), MbpStatusOk);
    CheckReply(&buf, "{\"ok\":false,\"error\":{\"code\":\"busy\",\"message\":\"client limit reached\"}}\n");
    MBP_CHECK_STATUS(MbpProto_WriteError(MbpStatusOs, &buf), MbpStatusOk);
    CheckReply(&buf, "{\"ok\":false,\"error\":{\"code\":\"internal\",\"message\":\"system call failed\"}}\n");
}

static void TestOverflow(void) {
    char text[40];
    MbpProtoBuf buf = { .text = text, .size = sizeof(text), .len = 0u };
    FillFixture();
    MBP_CHECK_STATUS(MbpProto_WriteValues(&cfg.stations[0], &copy, &buf), MbpStatusProtoOverflow);
    MBP_CHECK(buf.len < buf.size);
    MBP_CHECK_STATUS(MbpProto_WriteList(&cfg, NULL), MbpStatusParam);
}

int main(void) {
    MBP_CHECK_STATUS(MbpProto_Init(), MbpStatusOk);
    TestParseCases();
    TestParseIds();
    TestParseLongLines();
    TestWriters();
    TestWriteErrors();
    TestOverflow();
    MBP_CHECK_STATUS(MbpProto_Deinit(), MbpStatusOk);
    return MBP_TEST_RESULT();
}
