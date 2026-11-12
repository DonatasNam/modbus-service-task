#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <modbus.h>

#include "mbp_defs.h"
#include "mbp_link.h"

typedef struct {
    int errnoValue;
    MbpStatus status;
} ErrnoStatus;

static MbpStatus SetResponseTimeout(modbus_t *ctx, uint32_t timeoutMs);
static MbpStatus MapErrno(int errnoValue);

// libmodbus reports a Modbus exception code c as errno MODBUS_ENOBASE + c; a code it does not know as EMBBADEXC.
static const ErrnoStatus errnoStatuses[] = {
    { .errnoValue = EMBXILFUN, .status = MbpStatusLinkExcIllegalFunction },
    { .errnoValue = EMBXILADD, .status = MbpStatusLinkExcIllegalAddress },
    { .errnoValue = EMBXILVAL, .status = MbpStatusLinkExcIllegalValue },
    { .errnoValue = EMBXSFAIL, .status = MbpStatusLinkExcDeviceFailure },
    { .errnoValue = EMBXSBUSY, .status = MbpStatusLinkExcBusy },
    { .errnoValue = EMBXGPATH, .status = MbpStatusLinkExcGatewayPath },
    { .errnoValue = EMBXGTAR, .status = MbpStatusLinkExcGatewayTarget },
    { .errnoValue = EMBBADEXC, .status = MbpStatusLinkExcOther },
    { .errnoValue = EMBUNKEXC, .status = MbpStatusLinkExcOther },
    { .errnoValue = ETIMEDOUT, .status = MbpStatusLinkTimeout },
    { .errnoValue = EMBBADDATA, .status = MbpStatusLinkBadData },
    { .errnoValue = EMBBADCRC, .status = MbpStatusLinkBadData },
    { .errnoValue = EMBMDATA, .status = MbpStatusLinkBadData },
};

static MbpStatus SetResponseTimeout(modbus_t *ctx, uint32_t timeoutMs) {
    return (modbus_set_response_timeout(ctx, timeoutMs / MBP_MS_PER_SEC, (timeoutMs % MBP_MS_PER_SEC) * MBP_US_PER_MS) == 0) ? MbpStatusOk : MbpStatusParam;
}

static MbpStatus MapErrno(int errnoValue) {
    for (size_t i = 0u; i < (sizeof(errnoStatuses) / sizeof(errnoStatuses[0])); i++) {
        if (errnoStatuses[i].errnoValue == errnoValue) {
            return errnoStatuses[i].status;
        }
    }
    if ((errnoValue >= EMBXILFUN) && (errnoValue <= EMBXGTAR)) {
        return MbpStatusLinkExcOther;
    }
    return MbpStatusLinkIo;
}

MbpStatus MbpLink_Open(MbpLink *link, const char *host, uint16_t port, uint8_t unitId) {
    if ((link == NULL) || (host == NULL)) {
        return MbpStatusParam;
    }
    *link = (MbpLink) { .ctx = NULL, .isConnected = false };
    link->ctx = modbus_new_tcp(host, port);
    MbpStatus ret = (link->ctx != NULL) ? MbpStatusOk : MbpStatusLinkOpen;
    MBP_ASSERT_ERRNO(ret);

    ret = (modbus_set_slave(link->ctx, unitId) == 0) ? MbpStatusOk : MbpStatusLinkOpen;
    MBP_ASSERT_ERRNO(ret);

    ret = (modbus_set_error_recovery(link->ctx, MODBUS_ERROR_RECOVERY_NONE) == 0) ? MbpStatusOk : MbpStatusLinkOpen;
    MBP_ASSERT_ERRNO(ret);
    return ret;
}

MbpStatus MbpLink_Connect(MbpLink *link, uint32_t timeoutMs) {
    if ((link == NULL) || (link->ctx == NULL)) {
        return MbpStatusParam;
    }
    if (link->isConnected) {
        return MbpStatusOk;
    }
    MbpStatus ret = SetResponseTimeout(link->ctx, timeoutMs);
    MBP_ASSERT_SILENT(ret);

    ret = (modbus_connect(link->ctx) == 0) ? MbpStatusOk : MbpStatusLinkConnect;
    MBP_ASSERT_SILENT(ret);
    link->isConnected = true;
    return ret;
}

MbpStatus MbpLink_ReadRegs(MbpLink *link, uint8_t funcCode, uint16_t startAddr, uint16_t regCount, uint16_t *regs, uint32_t timeoutMs) {
    if ((link == NULL) || (link->ctx == NULL) || (regs == NULL) || (regCount == 0u) || (regCount > MBP_MAX_REGS)) {
        return MbpStatusParam;
    }
    if (!link->isConnected) {
        return MbpStatusLinkIo;
    }
    MbpStatus ret = SetResponseTimeout(link->ctx, timeoutMs);
    MBP_ASSERT_SILENT(ret);

    ret = (modbus_set_byte_timeout(link->ctx, 0u, 0u) == 0) ? MbpStatusOk : MbpStatusParam;
    MBP_ASSERT_SILENT(ret);

    int count = -1;
    if (funcCode == MODBUS_FC_READ_HOLDING_REGISTERS) {
        count = modbus_read_registers(link->ctx, startAddr, regCount, regs);
    } else if (funcCode == MODBUS_FC_READ_INPUT_REGISTERS) {
        count = modbus_read_input_registers(link->ctx, startAddr, regCount, regs);
    } else {
        return MbpStatusParam;
    }
    if (count < 0) {
        return MapErrno(errno);
    }
    return (count == (int) regCount) ? MbpStatusOk : MbpStatusLinkBadData;
}

MbpStatus MbpLink_Disconnect(MbpLink *link) {
    if (link == NULL) {
        return MbpStatusParam;
    }
    if (link->isConnected) {
        modbus_close(link->ctx);
        link->isConnected = false;
    }
    return MbpStatusOk;
}

MbpStatus MbpLink_Close(MbpLink *link) {
    if (link == NULL) {
        return MbpStatusParam;
    }
    MbpStatus ret = MbpLink_Disconnect(link);
    MBP_ASSERT(ret);
    if (link->ctx != NULL) {
        modbus_free(link->ctx);
        link->ctx = NULL;
    }
    return ret;
}
