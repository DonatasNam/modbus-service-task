#ifndef MBP_THREAD_H
#define MBP_THREAD_H

#include <pthread.h>

#include "mbp.h"

typedef void *(*MbpThreadEntryFn)(void *arg);

MbpStatus MbpThread_ToStatus(int osRet, MbpStatus failure);
MbpStatus MbpThread_Start(pthread_t *thread, MbpThreadEntryFn entry, void *arg);

#endif // MBP_THREAD_H
