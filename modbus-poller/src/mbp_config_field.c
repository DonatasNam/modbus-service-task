#include <arpa/inet.h>
#include <ctype.h>
#include <math.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mbp_config_field.h"
#include "mbp_defs.h"

#define UTF8_MAX_CODE_POINT 0x10FFFFu
#define UTF8_SURROGATE_FIRST 0xD800u
#define UTF8_SURROGATE_LAST 0xDFFFu

typedef struct {
    uint8_t leadMask;
    uint8_t leadBits;
    uint8_t len;
    uint32_t minCodePoint;
} Utf8Lead;

static size_t Utf8SequenceLen(const unsigned char *bytes);

// Multi-byte lead bytes; a minimum code point per length rules out overlong forms.
static const Utf8Lead utf8Leads[] = {
    { .leadMask = 0xE0u, .leadBits = 0xC0u, .len = 2u, .minCodePoint = 0x80u },
    { .leadMask = 0xF0u, .leadBits = 0xE0u, .len = 3u, .minCodePoint = 0x800u },
    { .leadMask = 0xF8u, .leadBits = 0xF0u, .len = 4u, .minCodePoint = 0x10000u },
};

// The length of the valid, non-control UTF-8 sequence at bytes, or 0. A NUL inside a sequence fails its check.
static size_t Utf8SequenceLen(const unsigned char *bytes) {
    if (bytes[0] < 0x80u) {
        return ((bytes[0] < MBP_CFG_FIELD_FIRST_PRINTABLE) || (bytes[0] == MBP_CFG_FIELD_DELETE)) ? 0u : 1u;
    }
    for (size_t i = 0u; i < (sizeof(utf8Leads) / sizeof(utf8Leads[0])); i++) {
        const Utf8Lead *lead = &utf8Leads[i];
        if ((bytes[0] & lead->leadMask) == lead->leadBits) {
            uint32_t codePoint = bytes[0] & (uint32_t) ~lead->leadMask & 0xFFu;
            for (size_t k = 1u; k < lead->len; k++) {
                if ((bytes[k] & 0xC0u) != 0x80u) {
                    return 0u;
                }
                codePoint = (codePoint << 6) | (bytes[k] & 0x3Fu);
            }
            const bool isSurrogate = (codePoint >= UTF8_SURROGATE_FIRST) && (codePoint <= UTF8_SURROGATE_LAST);
            if ((codePoint < lead->minCodePoint) || (codePoint > UTF8_MAX_CODE_POINT) || isSurrogate) {
                return 0u;
            }
            return lead->len;
        }
    }
    return 0u;
}

// Base 10, digits only: no sign, no space, so "010" is 10.
MbpStatus MbpCfgField_ParseUint(const char *text, uint32_t min, uint32_t max, uint32_t *value) {
    if ((text == NULL) || (value == NULL)) {
        return MbpStatusParam;
    }
    if (text[0] == '\0') {
        return MbpStatusCfgValue;
    }
    uint64_t number = 0u;
    for (const char *digit = text; *digit != '\0'; digit++) {
        if ((*digit < '0') || (*digit > '9')) {
            return MbpStatusCfgValue;
        }
        number = (number * 10u) + (uint64_t) (*digit - '0');
        if (number > UINT32_MAX) {
            return MbpStatusCfgRange;
        }
    }
    if ((number < min) || (number > max)) {
        return MbpStatusCfgRange;
    }
    *value = (uint32_t) number;
    return MbpStatusOk;
}

// strtod skips leading space, so a leading space is refused first; the whole string must be the number.
MbpStatus MbpCfgField_ParseScale(const char *text, double *value) {
    if ((text == NULL) || (value == NULL)) {
        return MbpStatusParam;
    }
    if ((text[0] == '\0') || (isspace((unsigned char) text[0]) != 0)) {
        return MbpStatusCfgValue;
    }
    char *end = NULL;
    const double number = strtod(text, &end);
    if (*end != '\0') {
        return MbpStatusCfgValue;
    }
    if (!isfinite(number)) {
        return MbpStatusCfgRange;
    }
    *value = number;
    return MbpStatusOk;
}

MbpStatus MbpCfgField_ParseBool(const char *text, bool *value) {
    if ((text == NULL) || (value == NULL)) {
        return MbpStatusParam;
    }
    if (strcmp(text, "true") == 0) {
        *value = true;
        return MbpStatusOk;
    }
    if (strcmp(text, "false") == 0) {
        *value = false;
        return MbpStatusOk;
    }
    return MbpStatusCfgValue;
}

MbpStatus MbpCfgField_CheckIpv4(const char *text) {
    struct in_addr addr;
    if (text == NULL) {
        return MbpStatusParam;
    }
    return (inet_pton(AF_INET, text, &addr) == 1) ? MbpStatusOk : MbpStatusCfgValue;
}

// Lengths are in bytes; the text must be valid UTF-8 without control characters.
MbpStatus MbpCfgField_CheckText(const char *text, size_t minLen, size_t maxLen) {
    if (text == NULL) {
        return MbpStatusParam;
    }
    const size_t len = strlen(text);
    if (len > maxLen) {
        return MbpStatusCfgTooLong;
    }
    if (len < minLen) {
        return MbpStatusCfgValue;
    }
    const unsigned char *bytes = (const unsigned char *) text;
    size_t offset = 0u;
    while (offset < len) {
        const size_t sequenceLen = Utf8SequenceLen(&bytes[offset]);
        if (sequenceLen == 0u) {
            return MbpStatusCfgValue;
        }
        offset += sequenceLen;
    }
    return MbpStatusOk;
}
