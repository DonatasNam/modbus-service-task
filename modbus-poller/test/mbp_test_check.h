#ifndef MBP_TEST_CHECK_H
#define MBP_TEST_CHECK_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "mbp.h"

static unsigned mbpTestFailures = 0u;

#define MBP_CHECK(cond)                                                                                                     \
    {                                                                                                                       \
        if (!(cond)) {                                                                                                      \
            (void) fprintf(stderr, "%s:%d check failed: %s\n", __FILE__, __LINE__, #cond);                                  \
            mbpTestFailures++;                                                                                              \
        }                                                                                                                   \
    }

#define MBP_CHECK_STATUS(expr, expected)                                                                                    \
    {                                                                                                                       \
        const MbpStatus checkRet = (expr);                                                                                  \
        const MbpStatus checkExpected = (expected);                                                                         \
        if (checkRet != checkExpected) {                                                                                    \
            (void) fprintf(stderr, "%s:%d %s: got \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #expr, Mbp_StatusStr(checkRet), Mbp_StatusStr(checkExpected)); \
            mbpTestFailures++;                                                                                              \
        }                                                                                                                   \
    }

#define MBP_CHECK_NEAR(actual, expected, relTol)                                                                            \
    {                                                                                                                       \
        const double checkActual = (actual);                                                                                \
        const double checkExpected = (expected);                                                                            \
        if (fabs(checkActual - checkExpected) > (fabs(checkExpected) * (relTol))) {                                         \
            (void) fprintf(stderr, "%s:%d %s: got %.17g, expected %.17g\n", __FILE__, __LINE__, #actual, checkActual, checkExpected); \
            mbpTestFailures++;                                                                                              \
        }                                                                                                                   \
    }

#define MBP_TEST_RESULT() ((mbpTestFailures == 0u) ? EXIT_SUCCESS : EXIT_FAILURE)

#endif // MBP_TEST_CHECK_H
