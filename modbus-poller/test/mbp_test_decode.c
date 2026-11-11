#include <stdint.h>

#include "mbp_decode.h"
#include "mbp_test_check.h"

#define FLOAT_TOL 1e-7
#define INT_TOL 1e-12

typedef struct {
    MbpDecType type;
    MbpDecOrder order;
    uint16_t regs[2];
    double scale;
    double expected;
} Vector;

static void TestVectors(void);
static void TestNotFinite(void);
static void TestBadInput(void);
static void TestRegCount(void);

// Expected values are the simulator's register rule: 123.25f is 0x42F68000, 3000000001 is 0xB2D05E01,
// -1001000 is 0xFFF0B9D8, -101 is 0xFF9B and 60001 is 0xEA61.
static const Vector vectors[] = {
    { .type = MbpDecTypeFloat32, .order = MbpDecOrderAbcd, .regs = { 0x42F6u, 0x8000u }, .scale = 1.0, .expected = 123.25 },
    { .type = MbpDecTypeFloat32, .order = MbpDecOrderBadc, .regs = { 0xF642u, 0x0080u }, .scale = 1.0, .expected = 123.25 },
    { .type = MbpDecTypeFloat32, .order = MbpDecOrderCdab, .regs = { 0x8000u, 0x42F6u }, .scale = 1.0, .expected = 123.25 },
    { .type = MbpDecTypeFloat32, .order = MbpDecOrderDcba, .regs = { 0x0080u, 0xF642u }, .scale = 1.0, .expected = 123.25 },
    { .type = MbpDecTypeFloat32, .order = MbpDecOrderAbcd, .regs = { 0x42F6u, 0x8000u }, .scale = 0.001, .expected = 0.12325 },
    { .type = MbpDecTypeUint32, .order = MbpDecOrderAbcd, .regs = { 0xB2D0u, 0x5E01u }, .scale = 1.0, .expected = 3000000001.0 },
    { .type = MbpDecTypeUint32, .order = MbpDecOrderBadc, .regs = { 0xD0B2u, 0x015Eu }, .scale = 1.0, .expected = 3000000001.0 },
    { .type = MbpDecTypeUint32, .order = MbpDecOrderCdab, .regs = { 0x5E01u, 0xB2D0u }, .scale = 1.0, .expected = 3000000001.0 },
    { .type = MbpDecTypeUint32, .order = MbpDecOrderDcba, .regs = { 0x015Eu, 0xD0B2u }, .scale = 1.0, .expected = 3000000001.0 },
    { .type = MbpDecTypeInt32, .order = MbpDecOrderAbcd, .regs = { 0xFFF0u, 0xB9D8u }, .scale = 1.0, .expected = -1001000.0 },
    { .type = MbpDecTypeInt32, .order = MbpDecOrderBadc, .regs = { 0xF0FFu, 0xD8B9u }, .scale = 1.0, .expected = -1001000.0 },
    { .type = MbpDecTypeInt32, .order = MbpDecOrderCdab, .regs = { 0xB9D8u, 0xFFF0u }, .scale = 1.0, .expected = -1001000.0 },
    { .type = MbpDecTypeInt32, .order = MbpDecOrderDcba, .regs = { 0xD8B9u, 0xF0FFu }, .scale = 10.0, .expected = -10010000.0 },
    { .type = MbpDecTypeInt16, .order = MbpDecOrderAbcd, .regs = { 0xFF9Bu, 0x0000u }, .scale = 1.0, .expected = -101.0 },
    { .type = MbpDecTypeInt16, .order = MbpDecOrderCdab, .regs = { 0xFF9Bu, 0x0000u }, .scale = 1.0, .expected = -101.0 },
    { .type = MbpDecTypeInt16, .order = MbpDecOrderBadc, .regs = { 0x9BFFu, 0x0000u }, .scale = 1.0, .expected = -101.0 },
    { .type = MbpDecTypeInt16, .order = MbpDecOrderDcba, .regs = { 0x9BFFu, 0x0000u }, .scale = 1.0, .expected = -101.0 },
    { .type = MbpDecTypeInt16, .order = MbpDecOrderAbcd, .regs = { 0x7FFFu, 0x0000u }, .scale = 1.0, .expected = 32767.0 },
    { .type = MbpDecTypeInt16, .order = MbpDecOrderAbcd, .regs = { 0x8000u, 0x0000u }, .scale = 1.0, .expected = -32768.0 },
    { .type = MbpDecTypeUint16, .order = MbpDecOrderAbcd, .regs = { 0xEA61u, 0x0000u }, .scale = 1.0, .expected = 60001.0 },
    { .type = MbpDecTypeUint16, .order = MbpDecOrderBadc, .regs = { 0x61EAu, 0x0000u }, .scale = 0.1, .expected = 6000.1 },
    { .type = MbpDecTypeUint16, .order = MbpDecOrderDcba, .regs = { 0x61EAu, 0x0000u }, .scale = 1.0, .expected = 60001.0 },
};

static void TestVectors(void) {
    for (size_t i = 0u; i < (sizeof(vectors) / sizeof(vectors[0])); i++) {
        const Vector *vector = &vectors[i];
        uint16_t regCount = 0u;
        double value = 0.0;
        MBP_CHECK_STATUS(MbpDec_RegCount(vector->type, &regCount), MbpStatusOk);
        MBP_CHECK_STATUS(MbpDec_ToValue(vector->regs, regCount, vector->type, vector->order, vector->scale, &value), MbpStatusOk);
        MBP_CHECK_NEAR(value, vector->expected, (vector->type == MbpDecTypeFloat32) ? FLOAT_TOL : INT_TOL);
    }
}

static void TestNotFinite(void) {
    const uint16_t quietNan[2] = { 0x7FC0u, 0x0000u };
    const uint16_t infinity[2] = { 0x7F80u, 0x0000u };
    const uint16_t largest[2] = { 0x7F7Fu, 0xFFFFu };
    double value = 7.0;
    MBP_CHECK_STATUS(MbpDec_ToValue(quietNan, 2u, MbpDecTypeFloat32, MbpDecOrderAbcd, 1.0, &value), MbpStatusDecNotFinite);
    MBP_CHECK_STATUS(MbpDec_ToValue(infinity, 2u, MbpDecTypeFloat32, MbpDecOrderAbcd, 1.0, &value), MbpStatusDecNotFinite);
    MBP_CHECK_STATUS(MbpDec_ToValue(largest, 2u, MbpDecTypeFloat32, MbpDecOrderAbcd, 1e300, &value), MbpStatusDecNotFinite);
    MBP_CHECK_NEAR(value, 7.0, INT_TOL);
    MBP_CHECK_STATUS(MbpDec_ToValue(largest, 2u, MbpDecTypeFloat32, MbpDecOrderAbcd, 1.0, &value), MbpStatusOk);
}

static void TestBadInput(void) {
    const uint16_t regs[2] = { 0u, 0u };
    double value = 0.0;
    MBP_CHECK_STATUS(MbpDec_ToValue(regs, 1u, MbpDecTypeFloat32, MbpDecOrderAbcd, 1.0, &value), MbpStatusDecType);
    MBP_CHECK_STATUS(MbpDec_ToValue(regs, 2u, MbpDecTypeInt16, MbpDecOrderAbcd, 1.0, &value), MbpStatusDecType);
    MBP_CHECK_STATUS(MbpDec_ToValue(regs, 1u, MbpDecTypeCount, MbpDecOrderAbcd, 1.0, &value), MbpStatusDecType);
    MBP_CHECK_STATUS(MbpDec_ToValue(regs, 1u, MbpDecTypeInt16, MbpDecOrderCount, 1.0, &value), MbpStatusDecType);
    MBP_CHECK_STATUS(MbpDec_ToValue(NULL, 1u, MbpDecTypeInt16, MbpDecOrderAbcd, 1.0, &value), MbpStatusParam);
    MBP_CHECK_STATUS(MbpDec_ToValue(regs, 1u, MbpDecTypeInt16, MbpDecOrderAbcd, 1.0, NULL), MbpStatusParam);
}

static void TestRegCount(void) {
    uint16_t count = 0u;
    MBP_CHECK_STATUS(MbpDec_RegCount(MbpDecTypeUint16, &count), MbpStatusOk);
    MBP_CHECK(count == MBP_DEC_REGS_16);
    MBP_CHECK_STATUS(MbpDec_RegCount(MbpDecTypeFloat32, &count), MbpStatusOk);
    MBP_CHECK(count == MBP_DEC_REGS_32);
    MBP_CHECK_STATUS(MbpDec_RegCount(MbpDecTypeCount, &count), MbpStatusDecType);
    MBP_CHECK_STATUS(MbpDec_RegCount(MbpDecTypeInt32, NULL), MbpStatusParam);
}

int main(void) {
    TestVectors();
    TestNotFinite();
    TestBadInput();
    TestRegCount();
    return MBP_TEST_RESULT();
}
