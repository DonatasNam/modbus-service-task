// Internal to modbus-poller. External users include mbp.h.
#ifndef MBP_LINK_H
#define MBP_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "mbp.h"

typedef struct {
    struct _modbus *ctx;                    // the tag libmodbus 3.1.6 typedefs as modbus_t; only mbp_link.c includes <modbus.h>
    bool isConnected;
} MbpLink;

MbpStatus MbpLink_Open(MbpLink *link, const char *host, uint16_t port, uint8_t unitId);
MbpStatus MbpLink_Connect(MbpLink *link, uint32_t timeoutMs);
MbpStatus MbpLink_ReadRegs(MbpLink *link, uint8_t funcCode, uint16_t startAddr, uint16_t regCount, uint16_t *regs, uint32_t timeoutMs);
MbpStatus MbpLink_Disconnect(MbpLink *link);
MbpStatus MbpLink_Close(MbpLink *link);

#endif // MBP_LINK_H
