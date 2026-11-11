#include <stddef.h>

#include "mbp.h"
#include "mbp_defs.h"

typedef struct {
    MbpStatus status;
    const char *text;
} StatusText;

// wire protocol v1: these texts travel as error and last_error; clients store these; additive only
static const StatusText statusTexts[] = {
    { .status = MbpStatusOk, .text = "ok" },
    { .status = MbpStatusParam, .text = "invalid parameter" },
    { .status = MbpStatusOs, .text = "system call failed" },
    { .status = MbpStatusThread, .text = "thread start failed" },
    { .status = MbpStatusStopped, .text = "stop requested" },
    { .status = MbpStatusCfgOpen, .text = "config file cannot be opened" },
    { .status = MbpStatusCfgRead, .text = "config file cannot be read" },
    { .status = MbpStatusCfgJson, .text = "config is not valid JSON" },
    { .status = MbpStatusCfgMissing, .text = "config field missing" },
    { .status = MbpStatusCfgType, .text = "config field has the wrong JSON type" },
    { .status = MbpStatusCfgValue, .text = "config value not allowed" },
    { .status = MbpStatusCfgRange, .text = "config value out of range" },
    { .status = MbpStatusCfgRegSize, .text = "config reg_size does not match data_type" },
    { .status = MbpStatusCfgDuplicate, .text = "config id already used" },
    { .status = MbpStatusCfgLimit, .text = "config entry count out of range" },
    { .status = MbpStatusCfgTooLong, .text = "config string too long" },
    { .status = MbpStatusDecNotFinite, .text = "decoded value is not finite" },
    { .status = MbpStatusDecType, .text = "data type, byte order or register count not supported" },
    { .status = MbpStatusLinkOpen, .text = "modbus context cannot be created" },
    { .status = MbpStatusLinkConnect, .text = "modbus connect failed" },
    { .status = MbpStatusLinkTimeout, .text = "modbus response timeout" },
    { .status = MbpStatusLinkIo, .text = "modbus connection I/O error" },
    { .status = MbpStatusLinkBadData, .text = "modbus reply malformed" },
    { .status = MbpStatusLinkExcIllegalFunction, .text = "modbus exception 01 illegal function" },
    { .status = MbpStatusLinkExcIllegalAddress, .text = "modbus exception 02 illegal data address" },
    { .status = MbpStatusLinkExcIllegalValue, .text = "modbus exception 03 illegal data value" },
    { .status = MbpStatusLinkExcDeviceFailure, .text = "modbus exception 04 server device failure" },
    { .status = MbpStatusLinkExcBusy, .text = "modbus exception 06 server device busy" },
    { .status = MbpStatusLinkExcGatewayPath, .text = "modbus exception 0A gateway path unavailable" },
    { .status = MbpStatusLinkExcGatewayTarget, .text = "modbus exception 0B gateway target device failed to respond" },
    { .status = MbpStatusLinkExcOther, .text = "modbus exception with another code" },
    { .status = MbpStatusIpcSocket, .text = "socket path unusable" },
    { .status = MbpStatusIpcBind, .text = "socket bind failed" },
    { .status = MbpStatusIpcInUse, .text = "socket path in use by another instance" },
    { .status = MbpStatusIpcBusy, .text = "client limit reached" },
    { .status = MbpStatusIpcTooLong, .text = "request line too long" },
    { .status = MbpStatusProtoJson, .text = "request is not valid JSON" },
    { .status = MbpStatusProtoField, .text = "request field missing or mistyped" },
    { .status = MbpStatusProtoDepth, .text = "request nesting too deep" },
    { .status = MbpStatusProtoArena, .text = "request too large to parse" },
    { .status = MbpStatusProtoUnknownCmd, .text = "unknown command" },
    { .status = MbpStatusProtoUnknownDevice, .text = "unknown device" },
    { .status = MbpStatusProtoUnknownPoint, .text = "unknown point" },
    { .status = MbpStatusProtoOverflow, .text = "reply does not fit its buffer" },
};

const char *Mbp_StatusStr(MbpStatus status) {
    for (size_t i = 0u; i < (sizeof(statusTexts) / sizeof(statusTexts[0])); i++) {
        if (statusTexts[i].status == status) {
            return statusTexts[i].text;
        }
    }
    return "unknown status";
}

MbpStatus Mbp_Init(const char *configPath, const char *socketPath) {
    if ((configPath == NULL) || (socketPath == NULL)) {
        return MbpStatusParam;
    }
    return MbpStatusOk;
}

MbpStatus Mbp_Deinit(void) {
    return MbpStatusOk;
}
