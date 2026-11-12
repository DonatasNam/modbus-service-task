#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "mbp_decode.h"
#include "mbp_defs.h"

typedef struct {
    bool isByteSwapped;
    bool isWordSwapped;
} OrderRule;

static uint16_t SwapBytes(uint16_t reg);
static uint32_t Assemble(const uint16_t *regs, uint16_t regCount, const OrderRule *rule);
static double RawToNumber(uint32_t raw, MbpDecType type);

static const uint16_t regCounts[MbpDecTypeCount] = {
    [MbpDecTypeInt16] = MBP_DEC_REGS_16,
    [MbpDecTypeUint16] = MBP_DEC_REGS_16,
    [MbpDecTypeInt32] = MBP_DEC_REGS_32,
    [MbpDecTypeUint32] = MBP_DEC_REGS_32,
    [MbpDecTypeFloat32] = MBP_DEC_REGS_32,
};

static const OrderRule orderRules[MbpDecOrderCount] = {
    [MbpDecOrderAbcd] = { .isByteSwapped = false, .isWordSwapped = false },
    [MbpDecOrderBadc] = { .isByteSwapped = true, .isWordSwapped = false },
    [MbpDecOrderCdab] = { .isByteSwapped = false, .isWordSwapped = true },
    [MbpDecOrderDcba] = { .isByteSwapped = true, .isWordSwapped = true },
};

MBP_CT_ASSERT(MBP_DEC_REGS_32 <= MBP_MAX_REGS, widest_type_fits_reg_buffer);
MBP_CT_ASSERT(sizeof(float) == 4u, float32_copies_from_uint32);

static uint16_t SwapBytes(uint16_t reg) {
    return (uint16_t) ((uint16_t) (reg << 8) | (uint16_t) (reg >> 8));
}

// One register: only the byte swap counts. Two: the word swap picks which register holds AB, then the byte swap.
static uint32_t Assemble(const uint16_t *regs, uint16_t regCount, const OrderRule *rule) {
    if (regCount == MBP_DEC_REGS_16) {
        return rule->isByteSwapped ? SwapBytes(regs[0]) : regs[0];
    }
    uint16_t high = rule->isWordSwapped ? regs[1] : regs[0];
    uint16_t low = rule->isWordSwapped ? regs[0] : regs[1];
    if (rule->isByteSwapped) {
        high = SwapBytes(high);
        low = SwapBytes(low);
    }
    return ((uint32_t) high << 16) | low;
}

static double RawToNumber(uint32_t raw, MbpDecType type) {
    float number = 0.0f;
    switch (type) {
    case MbpDecTypeInt16:
        return (double) ((int32_t) raw - (((raw & 0x8000u) != 0u) ? INT32_C(65536) : INT32_C(0)));
    case MbpDecTypeUint16:
        return (double) raw;
    case MbpDecTypeInt32:
        return (double) ((int64_t) raw - (((raw & 0x80000000u) != 0u) ? INT64_C(4294967296) : INT64_C(0)));
    case MbpDecTypeUint32:
        return (double) raw;
    case MbpDecTypeFloat32:
        (void) memcpy(&number, &raw, sizeof(number));
        return (double) number;
    case MbpDecTypeCount:
        break;
    }
    return NAN;
}

MbpStatus MbpDec_RegCount(MbpDecType type, uint16_t *count) {
    if (count == NULL) {
        return MbpStatusParam;
    }
    if ((unsigned) type >= (unsigned) MbpDecTypeCount) {
        return MbpStatusDecType;
    }
    *count = regCounts[type];
    return MbpStatusOk;
}

MbpStatus MbpDec_ToValue(const uint16_t *regs, uint16_t regCount, MbpDecType type, MbpDecOrder order, double scale, double *value) {
    if ((regs == NULL) || (value == NULL)) {
        return MbpStatusParam;
    }
    if (((unsigned) type >= (unsigned) MbpDecTypeCount) || ((unsigned) order >= (unsigned) MbpDecOrderCount) || (regCount != regCounts[type])) {
        return MbpStatusDecType;
    }
    const double raw = RawToNumber(Assemble(regs, regCount, &orderRules[order]), type);
    const double engineering = raw * scale;
    if (!isfinite(raw) || !isfinite(engineering)) {
        return MbpStatusDecNotFinite;
    }
    *value = engineering;
    return MbpStatusOk;
}
