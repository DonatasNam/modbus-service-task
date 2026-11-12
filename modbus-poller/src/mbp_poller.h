#ifndef MBP_POLLER_H
#define MBP_POLLER_H

#include "mbp.h"
#include "mbp_config.h"

MbpStatus MbpPoll_Start(const MbpCfg *cfg);
MbpStatus MbpPoll_Stop(void);

#endif // MBP_POLLER_H
