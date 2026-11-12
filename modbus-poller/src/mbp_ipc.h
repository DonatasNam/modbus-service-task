// Internal to modbus-poller. External users include mbp.h.
#ifndef MBP_IPC_H
#define MBP_IPC_H

#include "mbp.h"
#include "mbp_config.h"

MbpStatus MbpIpc_Start(const MbpCfg *cfg, const char *socketPath);
MbpStatus MbpIpc_Stop(void);

#endif // MBP_IPC_H
