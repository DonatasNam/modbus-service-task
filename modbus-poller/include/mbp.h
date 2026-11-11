#ifndef MBP_H
#define MBP_H

#define MBP_DEFAULT_CONFIG_PATH "/etc/modbus-poller/config.json"
#define MBP_DEFAULT_SOCKET_PATH "/run/modbus-poller/poller.sock"

// -1 .. -9 generic, -10 .. -29 config, -30 .. -39 decode, -40 .. -59 link, -60 .. -69 IPC, -70 .. -79 proto
typedef enum {
    MbpStatusOk = 0,
    MbpStatusParam = -1,
    MbpStatusOs = -2,                       // a system call failed; its log line carries errno
    MbpStatusThread = -3,                   // a thread did not start, or MBP_THREAD_STACK_BYTES is below PTHREAD_STACK_MIN
    MbpStatusStopped = -4,                  // a stop was requested; ends a worker, never sent to clients
    MbpStatusCfgOpen = -10,
    MbpStatusCfgRead = -11,
    MbpStatusCfgJson = -12,
    MbpStatusCfgMissing = -13,
    MbpStatusCfgType = -14,                 // a JSON value of the wrong kind (every leaf must be a string)
    MbpStatusCfgValue = -15,                // not a number, not in the allowed set, or not clean UTF-8
    MbpStatusCfgRange = -16,
    MbpStatusCfgRegSize = -17,
    MbpStatusCfgDuplicate = -18,
    MbpStatusCfgLimit = -19,                // an array with no entries or more than its MBP_MAX_* limit
    MbpStatusCfgTooLong = -20,
    MbpStatusDecNotFinite = -30,            // a NaN or infinite float32, or a raw x scale that overflows
    MbpStatusDecType = -31,                 // data type, byte order or register count outside the decode tables
    MbpStatusLinkOpen = -40,                // modbus_new_tcp, modbus_set_slave or the recovery mode refused
    MbpStatusLinkConnect = -41,
    MbpStatusLinkTimeout = -42,
    MbpStatusLinkIo = -43,
    MbpStatusLinkBadData = -44,             // a reply that does not match its request
    MbpStatusLinkExcIllegalFunction = -45,  // Modbus exception 01
    MbpStatusLinkExcIllegalAddress = -46,   // Modbus exception 02
    MbpStatusLinkExcIllegalValue = -47,     // Modbus exception 03
    MbpStatusLinkExcDeviceFailure = -48,    // Modbus exception 04
    MbpStatusLinkExcBusy = -49,             // Modbus exception 06
    MbpStatusLinkExcGatewayPath = -50,      // Modbus exception 0A
    MbpStatusLinkExcGatewayTarget = -51,    // Modbus exception 0B
    MbpStatusLinkExcOther = -52,            // any other exception code
    MbpStatusIpcSocket = -60,               // the path is too long, not a socket, or cannot be checked or removed; socket() or listen() failed
    MbpStatusIpcBind = -61,                 // bind, the stat of the bound inode, or chmod failed
    MbpStatusIpcInUse = -62,                // the path answers, or its probe failed other than refused; left in place
    MbpStatusIpcBusy = -63,
    MbpStatusIpcTooLong = -64,
    MbpStatusProtoJson = -70,
    MbpStatusProtoField = -71,
    MbpStatusProtoDepth = -72,
    MbpStatusProtoArena = -73,              // the line is over MBP_PROTO_LINE_MAX or exhausts the parse arena
    MbpStatusProtoUnknownCmd = -74,
    MbpStatusProtoUnknownDevice = -75,
    MbpStatusProtoUnknownPoint = -76,
    MbpStatusProtoOverflow = -77,           // a reply does not fit its MbpProtoBuf
} MbpStatus;

const char *Mbp_StatusStr(MbpStatus status);
MbpStatus Mbp_Init(const char *configPath, const char *socketPath);
MbpStatus Mbp_Deinit(void);

#endif // MBP_H
