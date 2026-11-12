#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>

#include "mbp_defs.h"
#include "mbp_thread.h"

static MbpStatus CheckStackSize(void);
static MbpStatus StartWith(pthread_t *thread, MbpThreadEntryFn entry, void *arg, pthread_attr_t *attr);

// PTHREAD_STACK_MIN is a sysconf() call in newer glibc, so the check runs at every start.
static MbpStatus CheckStackSize(void) {
    return ((size_t) MBP_THREAD_STACK_BYTES >= (size_t) PTHREAD_STACK_MIN) ? MbpStatusOk : MbpStatusThread;
}

static MbpStatus StartWith(pthread_t *thread, MbpThreadEntryFn entry, void *arg, pthread_attr_t *attr) {
    MbpStatus ret = MbpThread_ToStatus(pthread_attr_setstacksize(attr, MBP_THREAD_STACK_BYTES), MbpStatusThread);
    MBP_ASSERT_ERRNO(ret);

    ret = MbpThread_ToStatus(pthread_create(thread, attr, entry, arg), MbpStatusThread);
    MBP_ASSERT_ERRNO(ret);
    return ret;
}

MbpStatus MbpThread_ToStatus(int osRet, MbpStatus failure) {
    if (osRet == 0) {
        return MbpStatusOk;
    }
    errno = osRet;
    return failure;
}

MbpStatus MbpThread_Start(pthread_t *thread, MbpThreadEntryFn entry, void *arg) {
    pthread_attr_t attr;
    if ((thread == NULL) || (entry == NULL)) {
        return MbpStatusParam;
    }
    MbpStatus ret = CheckStackSize();
    MBP_ASSERT(ret);

    ret = MbpThread_ToStatus(pthread_attr_init(&attr), MbpStatusThread);
    MBP_ASSERT_ERRNO(ret);

    ret = StartWith(thread, entry, arg, &attr);
    (void) pthread_attr_destroy(&attr);
    return ret;
}
