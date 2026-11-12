#ifndef MBP_IPC_PATH_H
#define MBP_IPC_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/un.h>

#include "mbp.h"

#define MBP_IPC_PATH_BYTES sizeof(((struct sockaddr_un *) NULL)->sun_path)

typedef struct {
    char socketPath[MBP_IPC_PATH_BYTES];
    bool isBound;
    dev_t boundDev;
    ino_t boundIno;
} MbpIpcPath;

MbpStatus MbpIpcPath_Listen(MbpIpcPath *path, const char *socketPath, int *listenFd);
MbpStatus MbpIpcPath_Unlink(MbpIpcPath *path);

#endif // MBP_IPC_PATH_H
