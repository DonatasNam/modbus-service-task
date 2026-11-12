#ifndef MBP_CONFIG_FIELD_H
#define MBP_CONFIG_FIELD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mbp.h"

#define MBP_CFG_FIELD_FIRST_PRINTABLE 0x20u
#define MBP_CFG_FIELD_DELETE 0x7Fu

MbpStatus MbpCfgField_ParseUint(const char *text, uint32_t min, uint32_t max, uint32_t *value);
MbpStatus MbpCfgField_ParseScale(const char *text, double *value);
MbpStatus MbpCfgField_ParseBool(const char *text, bool *value);
MbpStatus MbpCfgField_CheckIpv4(const char *text);
MbpStatus MbpCfgField_CheckText(const char *text, size_t minLen, size_t maxLen);

#endif // MBP_CONFIG_FIELD_H
