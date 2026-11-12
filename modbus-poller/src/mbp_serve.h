#ifndef MBP_SERVE_H
#define MBP_SERVE_H

#include <stddef.h>

#include "mbp.h"
#include "mbp_config.h"
#include "mbp_proto.h"

MbpStatus MbpServe_Line(const MbpCfg *cfg, const char *line, size_t len, MbpProtoBuf *reply);

#endif // MBP_SERVE_H
