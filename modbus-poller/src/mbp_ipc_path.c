#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "mbp_defs.h"
#include "mbp_ipc_path.h"

#define LISTEN_BACKLOG 16
#define SOCKET_MODE 0660

static MbpStatus CopyPath(MbpIpcPath *path, const char *socketPath);
static MbpStatus ProbePath(const MbpIpcPath *path);
static MbpStatus ProbeSocket(const MbpIpcPath *path);
static MbpStatus Bind(MbpIpcPath *path, int listenFd);
static void FillAddr(const MbpIpcPath *path, struct sockaddr_un *addr);

static MbpStatus CopyPath(MbpIpcPath *path, const char *socketPath) {
    const size_t pathLen = strlen(socketPath);
    const MbpStatus ret = ((pathLen > 0u) && (pathLen < sizeof(path->socketPath))) ? MbpStatusOk : MbpStatusIpcSocket;
    MBP_ASSERT(ret);
    (void) memset(path, 0, sizeof(*path));
    (void) memcpy(path->socketPath, socketPath, pathLen);
    return ret;
}

// One instance per path: only a refused probe means a stale file. An answer, or any other error (a stalled
// instance's full backlog gives EAGAIN), leaves the path alone and fails the start.
static MbpStatus ProbePath(const MbpIpcPath *path) {
    struct stat info;
    if (lstat(path->socketPath, &info) != 0) {
        const int statErrno = errno;
        if (statErrno == ENOENT) {
            return MbpStatusOk;
        }
        MBP_LOG_ERR("socket %s: %s", path->socketPath, strerror(statErrno));
        return MbpStatusIpcSocket;
    }
    if (!S_ISSOCK(info.st_mode)) {
        MBP_LOG_ERR("socket %s: exists and is not a socket", path->socketPath);
        return MbpStatusIpcSocket;
    }
    return ProbeSocket(path);
}

static MbpStatus ProbeSocket(const MbpIpcPath *path) {
    struct sockaddr_un addr;
    FillAddr(path, &addr);
    const int probeFd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    MbpStatus ret = (probeFd >= 0) ? MbpStatusOk : MbpStatusOs;
    MBP_ASSERT_ERRNO(ret);

    const int connectRet = connect(probeFd, (const struct sockaddr *) &addr, sizeof(addr));
    const int connectErrno = errno;
    ret = (close(probeFd) == 0) ? MbpStatusOk : MbpStatusOs;
    MBP_ASSERT_ERRNO(ret);
    if (connectRet == 0) {
        MBP_LOG_ERR("socket %s: another instance answers", path->socketPath);
        return MbpStatusIpcInUse;
    }
    if (connectErrno != ECONNREFUSED) {
        MBP_LOG_ERR("socket %s: probe failed, left in place: %s", path->socketPath, strerror(connectErrno));
        return MbpStatusIpcInUse;
    }
    MBP_LOG_WARN("socket %s: stale, removed", path->socketPath);
    ret = (unlink(path->socketPath) == 0) ? MbpStatusOk : MbpStatusIpcSocket;
    MBP_ASSERT_ERRNO(ret);
    return ret;
}

// The inode is recorded right after bind, so MbpIpcPath_Unlink never removes a path another instance has taken
// over. Nobody can connect before listen(), so the mode is set before anyone can use the socket.
static MbpStatus Bind(MbpIpcPath *path, int listenFd) {
    struct sockaddr_un addr;
    struct stat info;
    FillAddr(path, &addr);
    MbpStatus ret = (bind(listenFd, (const struct sockaddr *) &addr, sizeof(addr)) == 0) ? MbpStatusOk : MbpStatusIpcBind;
    MBP_ASSERT_ERRNO(ret);

    ret = (stat(path->socketPath, &info) == 0) ? MbpStatusOk : MbpStatusIpcBind;
    MBP_ASSERT_ERRNO(ret);
    path->boundDev = info.st_dev;
    path->boundIno = info.st_ino;
    path->isBound = true;

    ret = (chmod(path->socketPath, SOCKET_MODE) == 0) ? MbpStatusOk : MbpStatusIpcBind;
    MBP_ASSERT_ERRNO(ret);
    return ret;
}

static void FillAddr(const MbpIpcPath *path, struct sockaddr_un *addr) {
    (void) memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    (void) memcpy(addr->sun_path, path->socketPath, sizeof(addr->sun_path));
}

// *listenFd is written as soon as the socket exists, so the caller's stop closes it whatever step fails after.
MbpStatus MbpIpcPath_Listen(MbpIpcPath *path, const char *socketPath, int *listenFd) {
    if ((path == NULL) || (socketPath == NULL) || (listenFd == NULL)) {
        return MbpStatusParam;
    }
    MbpStatus ret = CopyPath(path, socketPath);
    MBP_ASSERT(ret);

    ret = ProbePath(path);
    MBP_ASSERT(ret);

    *listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    ret = (*listenFd >= 0) ? MbpStatusOk : MbpStatusIpcSocket;
    MBP_ASSERT_ERRNO(ret);

    ret = Bind(path, *listenFd);
    MBP_ASSERT(ret);

    ret = (listen(*listenFd, LISTEN_BACKLOG) == 0) ? MbpStatusOk : MbpStatusIpcSocket;
    MBP_ASSERT_ERRNO(ret);
    return ret;
}

// Removes the path only while it is still the inode this instance bound. Safe when never bound or run twice.
MbpStatus MbpIpcPath_Unlink(MbpIpcPath *path) {
    struct stat info;
    if (path == NULL) {
        return MbpStatusParam;
    }
    if (!path->isBound) {
        return MbpStatusOk;
    }
    path->isBound = false;
    if (stat(path->socketPath, &info) != 0) {
        return (errno == ENOENT) ? MbpStatusOk : MbpStatusOs;
    }
    if ((info.st_dev != path->boundDev) || (info.st_ino != path->boundIno)) {
        return MbpStatusOk;
    }
    return (unlink(path->socketPath) == 0) ? MbpStatusOk : MbpStatusOs;
}
