// Internal to modbus-poller. External users include mbp.h.
#ifndef MBP_DECODE_H
#define MBP_DECODE_H

#include <stdint.h>

#include "mbp.h"

#define MBP_DEC_REGS_16 1u
#define MBP_DEC_REGS_32 2u

typedef enum {
    MbpDecTypeInt16 = 0,
    MbpDecTypeUint16,
    MbpDecTypeInt32,
    MbpDecTypeUint32,
    MbpDecTypeFloat32,
    MbpDecTypeCount,
} MbpDecType;

// A..D are the value's big-endian bytes; the name is their order on the wire (ABCD: reg0 = AB, reg1 = CD).
typedef enum {
    MbpDecOrderAbcd = 0,
    MbpDecOrderBadc,
    MbpDecOrderCdab,
    MbpDecOrderDcba,
    MbpDecOrderCount,
} MbpDecOrder;

MbpStatus MbpDec_RegCount(MbpDecType type, uint16_t *count);
MbpStatus MbpDec_ToValue(const uint16_t *regs, uint16_t regCount, MbpDecType type, MbpDecOrder order, double scale, double *value);

#endif // MBP_DECODE_H
