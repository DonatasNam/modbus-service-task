// Internal to modbus-poller. External users include mbp.h.
#ifndef MBP_DEFS_H
#define MBP_DEFS_H

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "mbp.h"

#define MBP_MAX_CONNS 16u
#define MBP_MAX_STATIONS 32u
#define MBP_MAX_POINTS 64u
#define MBP_MAX_REGS 2u
#define MBP_THREAD_STACK_BYTES 65536u
#define MBP_MS_PER_SEC 1000u
#define MBP_US_PER_MS 1000u
#define MBP_NS_PER_MS 1000000u

#ifndef MBP_VERBOSE_ERRORS
#define MBP_VERBOSE_ERRORS 1
#endif

#define MBP_CT_ASSERT(cond, tag) _Static_assert((cond), #tag)

// One line per call, written whole: stderr is unbuffered and glibc writes one fprintf in one go. The trailing ""
// keeps the variadic list non-empty for a format without arguments. A failed write to stderr has nowhere to go.
#define MBP_LOG_LINE(prio, fmt, ...) (void) fprintf(stderr, prio fmt "%s\n", __VA_ARGS__)
#define MBP_LOG_ERR(...) MBP_LOG_LINE("<3>", __VA_ARGS__, "")
#define MBP_LOG_WARN(...) MBP_LOG_LINE("<4>", __VA_ARGS__, "")
#define MBP_LOG_INFO(...) MBP_LOG_LINE("<6>", __VA_ARGS__, "")

#if MBP_VERBOSE_ERRORS
#define MBP_ERR(ret) MBP_LOG_ERR("%s:%d %s", __func__, __LINE__, Mbp_StatusStr(ret))
#define MBP_ERR_ERRNO(ret, err) MBP_LOG_ERR("%s:%d %s: %s", __func__, __LINE__, Mbp_StatusStr(ret), strerror(err))
#else
#define MBP_ERR(ret) ((void) (ret))
#define MBP_ERR_ERRNO(ret, err) ((void) (ret), (void) (err))
#endif

// MBP_ASSERT logs and returns; MBP_ASSERT_SILENT returns without a log (poll and IPC hot paths); MBP_ASSERT_ERRNO
// logs with strerror(errno) taken right after the step; MBP_ASSERT_NON_FATAL logs, keeps the first failure, goes on.
#define MBP_ASSERT(ret)                                                 \
    {                                                                   \
        const MbpStatus assertRet = (ret);                              \
        if (assertRet != MbpStatusOk) {                                 \
            MBP_ERR(assertRet);                                         \
            return assertRet;                                           \
        }                                                               \
    }

#define MBP_ASSERT_SILENT(ret)                                          \
    {                                                                   \
        const MbpStatus assertRet = (ret);                              \
        if (assertRet != MbpStatusOk) {                                 \
            return assertRet;                                           \
        }                                                               \
    }

#define MBP_ASSERT_ERRNO(ret)                                           \
    {                                                                   \
        const MbpStatus assertRet = (ret);                              \
        const int assertErrno = errno;                                  \
        if (assertRet != MbpStatusOk) {                                 \
            MBP_ERR_ERRNO(assertRet, assertErrno);                      \
            return assertRet;                                           \
        }                                                               \
    }

#define MBP_ASSERT_NON_FATAL(ret, firstRet)                             \
    {                                                                   \
        const MbpStatus assertRet = (ret);                              \
        MbpStatus *const assertFirst = &(firstRet);                     \
        if (assertRet != MbpStatusOk) {                                 \
            MBP_ERR(assertRet);                                         \
            if (*assertFirst == MbpStatusOk) {                          \
                *assertFirst = assertRet;                               \
            }                                                           \
        }                                                               \
    }

#endif // MBP_DEFS_H
