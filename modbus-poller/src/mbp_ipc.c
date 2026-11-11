#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "mbp_clock.h"
#include "mbp_config.h"
#include "mbp_defs.h"
#include "mbp_ipc.h"
#include "mbp_ipc_path.h"
#include "mbp_proto.h"
#include "mbp_serve.h"
#include "mbp_thread.h"

#define MAX_CLIENTS 32u
#define IDLE_CLOSE_MS 60000u
#define DRAIN_MAX 65536u
#define DRAIN_CHUNK_BYTES 4096u
#define ACCEPT_BURST_MAX (MAX_CLIENTS + 16u)
#define STOP_SLOT 0u
#define LISTEN_SLOT 1u
#define FIRST_CLIENT_SLOT 2u
#define POLL_SLOTS (MAX_CLIENTS + FIRST_CLIENT_SLOT)
// A point with its strings empty: 11 + 8 + 11 + 11 + 10 bytes of keys and quotes up to the value, a %.10g number
// within 24, 6 + 20 for the timestamp, 12 + 5 for the quality, 10 + 2 + 64 for the error text, 2 for "}" and ",".
#define POINT_FRAME_BYTES 224u
// {"ok":true,"result":{"id":"","points":[]}} and the newline: 43 bytes.
#define REPLY_ENVELOPE_BYTES 64u
#define REPLY_BYTES (REPLY_ENVELOPE_BYTES + (MBP_PROTO_ESCAPE_GROWTH * MBP_CFG_ID_MAX) + (MBP_MAX_POINTS * (POINT_FRAME_BYTES + (MBP_PROTO_ESCAPE_GROWTH * ((2u * MBP_CFG_ID_MAX) + MBP_CFG_DESCR_MAX + MBP_CFG_UNITS_MAX)))))
// A list entry with its strings empty, the largest port, unit and point count: 98 bytes.
#define LIST_ENTRY_FRAME_BYTES 128u
#define LIST_BYTES (REPLY_ENVELOPE_BYTES + (MBP_MAX_STATIONS * (LIST_ENTRY_FRAME_BYTES + INET_ADDRSTRLEN + (MBP_PROTO_ESCAPE_GROWTH * (MBP_CFG_ID_MAX + (2u * MBP_CFG_NAME_MAX))))))

typedef struct {
    int fd;
    bool isOpen;
    uint64_t lastRequestMs;                 // monotonic; the idle deadline counts from here
    size_t inLen;
    char in[MBP_PROTO_LINE_MAX + 1u];       // one request line and its newline
} Client;

// The IPC thread owns the clients, the buffers and the listen socket once it runs; the main thread sets them up
// in MbpIpc_Start before the thread starts and tears them down in MbpIpc_Stop after the join.
typedef struct {
    const MbpCfg *cfg;
    int listenFd;
    int stopFd;
    bool isThreadStarted;
    bool hasReportedLimit;
    bool hasReportedSendCap;
    pthread_t thread;
    MbpIpcPath path;
    MbpProtoBuf reply;
    Client clients[MAX_CLIENTS];
    char replyText[REPLY_BYTES];
    char drain[DRAIN_CHUNK_BYTES];
} Ipc;

static MbpStatus ResetState(const MbpCfg *cfg);
static MbpStatus WakeThread(void);
static MbpStatus CloseFd(int *fd);
static MbpStatus PollLoop(void);
static MbpStatus PollOnce(void);
static MbpStatus AcceptClients(uint64_t nowMs);
static MbpStatus AddClient(int fd, uint64_t nowMs);
static MbpStatus SetSendBuffer(int fd);
static MbpStatus ReceiveRequests(Client *client, uint64_t nowMs);
static MbpStatus ReplyToEachLine(Client *client, uint64_t nowMs);
static MbpStatus ReplyTo(Client *client, size_t lineLen);
static MbpStatus DropClient(Client *client, MbpStatus error);
static MbpStatus CloseClient(int fd, MbpStatus error);
static MbpStatus ReapIdle(uint64_t nowMs);
static nfds_t FillPollSet(struct pollfd *fds, uint16_t *slotClients);
static int IdleTimeoutMs(uint64_t nowMs);
static void *IpcMain(void *arg);

static Ipc ipc = { .listenFd = -1, .stopFd = -1 };

MBP_CT_ASSERT(LIST_BYTES <= REPLY_BYTES, list_reply_fits_reply_buffer);
MBP_CT_ASSERT(POLL_SLOTS <= UINT16_MAX, poll_slot_fits_u16);
MBP_CT_ASSERT(REPLY_BYTES <= INT_MAX, reply_bytes_fit_sndbuf_int);
MBP_CT_ASSERT(IDLE_CLOSE_MS <= INT_MAX, idle_timeout_fits_poll_int);

static MbpStatus ResetState(const MbpCfg *cfg) {
    ipc.cfg = cfg;
    ipc.reply = (MbpProtoBuf) { .text = ipc.replyText, .size = sizeof(ipc.replyText), .len = 0u };
    for (uint16_t i = 0u; i < MAX_CLIENTS; i++) {
        ipc.clients[i] = (Client) { .fd = -1, .isOpen = false, .lastRequestMs = 0u, .inLen = 0u };
    }
    return MbpStatusOk;
}

static MbpStatus WakeThread(void) {
    const uint64_t one = 1u;
    return (write(ipc.stopFd, &one, sizeof(one)) == (ssize_t) sizeof(one)) ? MbpStatusOk : MbpStatusOs;
}

// Linux releases the descriptor even when close() reports an error, so the slot is marked closed either way.
static MbpStatus CloseFd(int *fd) {
    if (*fd < 0) {
        return MbpStatusOk;
    }
    const int closeRet = close(*fd);
    *fd = -1;
    return (closeRet == 0) ? MbpStatusOk : MbpStatusOs;
}

// Ends with Ok on a stop, with the failure that ended the server otherwise.
static MbpStatus PollLoop(void) {
    MbpStatus ret = MbpStatusOk;
    while (ret == MbpStatusOk) {
        ret = PollOnce();
    }
    return (ret == MbpStatusStopped) ? MbpStatusOk : ret;
}

static MbpStatus PollOnce(void) {
    struct pollfd fds[POLL_SLOTS];
    uint16_t slotClients[POLL_SLOTS];
    uint64_t nowMs = 0u;
    MbpStatus ret = MbpClock_MonoMs(&nowMs);
    MBP_ASSERT_SILENT(ret);

    const nfds_t count = FillPollSet(fds, slotClients);
    if (poll(fds, count, IdleTimeoutMs(nowMs)) < 0) {
        return (errno == EINTR) ? MbpStatusOk : MbpStatusOs;
    }
    if (fds[STOP_SLOT].revents != 0) {
        return MbpStatusStopped;
    }

    ret = MbpClock_MonoMs(&nowMs);
    MBP_ASSERT_SILENT(ret);
    for (nfds_t slot = FIRST_CLIENT_SLOT; slot < count; slot++) {
        if (fds[slot].revents != 0) {
            (void) ReceiveRequests(&ipc.clients[slotClients[slot]], nowMs);         // a failure has closed that one client
        }
    }
    if (fds[LISTEN_SLOT].revents != 0) {
        (void) AcceptClients(nowMs);        // a failure belongs to one connection attempt; the next poll tries again
    }
    return ReapIdle(nowMs);
}

static MbpStatus AcceptClients(uint64_t nowMs) {
    MbpStatus ret = MbpStatusOk;
    for (uint32_t i = 0u; i < ACCEPT_BURST_MAX; i++) {
        const int fd = accept4(ipc.listenFd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            return ret;
        }
        ret = AddClient(fd, nowMs);
        MBP_ASSERT_SILENT(ret);
    }
    return ret;
}

static MbpStatus AddClient(int fd, uint64_t nowMs) {
    Client *client = NULL;
    for (uint16_t i = 0u; (i < MAX_CLIENTS) && (client == NULL); i++) {
        if (!ipc.clients[i].isOpen) {
            client = &ipc.clients[i];
        }
    }
    if (client == NULL) {
        if (!ipc.hasReportedLimit) {
            MBP_LOG_WARN("ipc: client limit %u reached, further clients get busy", MAX_CLIENTS);
            ipc.hasReportedLimit = true;
        }
        return CloseClient(fd, MbpStatusIpcBusy);
    }
    if (SetSendBuffer(fd) != MbpStatusOk) {
        return CloseClient(fd, MbpStatusOk);
    }
    *client = (Client) { .fd = fd, .isOpen = true, .lastRequestMs = nowMs, .inLen = 0u };
    return MbpStatusOk;
}

// A request/response client always has room for one whole reply, so a send that does not fit means the client
// stopped reading.
static MbpStatus SetSendBuffer(int fd) {
    int size = 0;
    socklen_t sizeLen = sizeof(size);
    MbpStatus ret = (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, &sizeLen) == 0) ? MbpStatusOk : MbpStatusOs;
    MBP_ASSERT_SILENT(ret);
    if ((size_t) size >= REPLY_BYTES) {
        return ret;
    }
    const int wanted = (int) REPLY_BYTES;
    ret = (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &wanted, sizeof(wanted)) == 0) ? MbpStatusOk : MbpStatusOs;
    MBP_ASSERT_SILENT(ret);
    sizeLen = sizeof(size);
    ret = (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, &sizeLen) == 0) ? MbpStatusOk : MbpStatusOs;
    MBP_ASSERT_SILENT(ret);
    if (((size_t) size < REPLY_BYTES) && !ipc.hasReportedSendCap) {
        MBP_LOG_WARN("ipc: SO_SNDBUF capped at %d bytes, below the largest reply of %u bytes", size, (unsigned) REPLY_BYTES);
        ipc.hasReportedSendCap = true;
    }
    return ret;
}

// One recv per wake-up, so a client that floods requests cannot hold the thread from the others.
static MbpStatus ReceiveRequests(Client *client, uint64_t nowMs) {
    const ssize_t got = recv(client->fd, &client->in[client->inLen], sizeof(client->in) - client->inLen, 0);
    if (got == 0) {
        return DropClient(client, MbpStatusOk);
    }
    if (got < 0) {
        return ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR)) ? MbpStatusOk : DropClient(client, MbpStatusOk);
    }
    client->inLen += (size_t) got;
    return ReplyToEachLine(client, nowMs);
}

// A full buffer without a newline is a line over MBP_PROTO_LINE_MAX.
static MbpStatus ReplyToEachLine(Client *client, uint64_t nowMs) {
    const char *newline = memchr(client->in, '\n', client->inLen);
    while (newline != NULL) {
        const size_t lineLen = (size_t) (newline - client->in);
        MbpStatus ret = ReplyTo(client, lineLen);
        MBP_ASSERT_SILENT(ret);
        if (!client->isOpen) {
            return ret;
        }
        client->lastRequestMs = nowMs;
        client->inLen -= lineLen + 1u;
        (void) memmove(client->in, &client->in[lineLen + 1u], client->inLen);
        newline = memchr(client->in, '\n', client->inLen);
    }
    return (client->inLen == sizeof(client->in)) ? DropClient(client, MbpStatusIpcTooLong) : MbpStatusOk;
}

// MbpServe_Line's one caller: a failed request always gets its error reply. A reply that cannot go out whole drops
// the client.
static MbpStatus ReplyTo(Client *client, size_t lineLen) {
    MbpStatus ret = MbpServe_Line(ipc.cfg, client->in, lineLen, &ipc.reply);
    if (ret != MbpStatusOk) {
        ret = MbpProto_WriteError(ret, &ipc.reply);
    }
    const ssize_t sent = (ret == MbpStatusOk) ? send(client->fd, ipc.reply.text, ipc.reply.len, MSG_NOSIGNAL | MSG_DONTWAIT) : -1;
    if (sent != (ssize_t) ipc.reply.len) {
        return DropClient(client, MbpStatusOk);
    }
    return MbpStatusOk;
}

static MbpStatus DropClient(Client *client, MbpStatus error) {
    const int fd = client->fd;
    *client = (Client) { .fd = -1, .isOpen = false, .lastRequestMs = 0u, .inLen = 0u };
    return CloseClient(fd, error);
}

// The one path for every close the service starts: the error line if there is one, then SHUT_WR, a bounded drain
// of what the client already sent, then close. Linux AF_UNIX resets the peer when a socket closes with unread data;
// after the drain the client sees a clean EOF. The error send is best effort: the close goes on either way.
static MbpStatus CloseClient(int fd, MbpStatus error) {
    int closeFd = fd;
    if ((error != MbpStatusOk) && (MbpProto_WriteError(error, &ipc.reply) == MbpStatusOk)) {
        (void) send(fd, ipc.reply.text, ipc.reply.len, MSG_NOSIGNAL | MSG_DONTWAIT);         // best effort, see above
    }
    (void) shutdown(fd, SHUT_WR);           // fails only on a peer already gone, which the close handles too
    size_t drained = 0u;
    ssize_t got = recv(fd, ipc.drain, sizeof(ipc.drain), MSG_DONTWAIT);
    while ((got > 0) && (drained < DRAIN_MAX)) {
        drained += (size_t) got;
        got = recv(fd, ipc.drain, sizeof(ipc.drain), MSG_DONTWAIT);
    }
    return CloseFd(&closeFd);
}

static MbpStatus ReapIdle(uint64_t nowMs) {
    for (uint16_t i = 0u; i < MAX_CLIENTS; i++) {
        Client *client = &ipc.clients[i];
        if (client->isOpen && ((int64_t) (nowMs - client->lastRequestMs) >= (int64_t) IDLE_CLOSE_MS)) {
            (void) DropClient(client, MbpStatusOk);         // the slot is free and the descriptor released either way
        }
    }
    return MbpStatusOk;
}

// The stop eventfd, the listen socket, then every open client; slotClients maps a client slot to its client.
static nfds_t FillPollSet(struct pollfd *fds, uint16_t *slotClients) {
    nfds_t count = FIRST_CLIENT_SLOT;
    fds[STOP_SLOT] = (struct pollfd) { .fd = ipc.stopFd, .events = POLLIN, .revents = 0 };
    fds[LISTEN_SLOT] = (struct pollfd) { .fd = ipc.listenFd, .events = POLLIN, .revents = 0 };
    for (uint16_t i = 0u; i < MAX_CLIENTS; i++) {
        if (ipc.clients[i].isOpen) {
            fds[count] = (struct pollfd) { .fd = ipc.clients[i].fd, .events = POLLIN, .revents = 0 };
            slotClients[count] = i;
            count++;
        }
    }
    return count;
}

// The poll timeout is the earliest idle deadline, or none while no client is connected.
static int IdleTimeoutMs(uint64_t nowMs) {
    int64_t earliestMs = -1;
    for (uint16_t i = 0u; i < MAX_CLIENTS; i++) {
        if (ipc.clients[i].isOpen) {
            int64_t leftMs = (int64_t) (ipc.clients[i].lastRequestMs + IDLE_CLOSE_MS - nowMs);
            leftMs = (leftMs < 0) ? 0 : leftMs;
            earliestMs = ((earliestMs < 0) || (leftMs < earliestMs)) ? leftMs : earliestMs;
        }
    }
    return (int) earliestMs;
}

static void *IpcMain(void *arg) {
    (void) arg;
    const MbpStatus ret = PollLoop();
    if (ret != MbpStatusOk) {
        MBP_LOG_ERR("ipc: server stopped: %s", Mbp_StatusStr(ret));
    }
    return NULL;
}

MbpStatus MbpIpc_Start(const MbpCfg *cfg, const char *socketPath) {
    if ((cfg == NULL) || (socketPath == NULL)) {
        return MbpStatusParam;
    }
    MbpStatus ret = ResetState(cfg);
    MBP_ASSERT(ret);

    ipc.stopFd = eventfd(0u, EFD_CLOEXEC | EFD_NONBLOCK);
    ret = (ipc.stopFd >= 0) ? MbpStatusOk : MbpStatusOs;
    MBP_ASSERT_ERRNO(ret);

    ret = MbpIpcPath_Listen(&ipc.path, socketPath, &ipc.listenFd);
    MBP_ASSERT(ret);

    ret = MbpThread_Start(&ipc.thread, IpcMain, NULL);
    MBP_ASSERT(ret);
    ipc.isThreadStarted = true;
    return ret;
}

// Safe when never started, partly started or already stopped: closes only open descriptors, and the path goes only
// while it is still the inode this instance bound.
MbpStatus MbpIpc_Stop(void) {
    MbpStatus firstRet = MbpStatusOk;
    if (ipc.isThreadStarted) {
        MBP_ASSERT_NON_FATAL(WakeThread(), firstRet);
        MBP_ASSERT_NON_FATAL(MbpThread_ToStatus(pthread_join(ipc.thread, NULL), MbpStatusOs), firstRet);
        ipc.isThreadStarted = false;
    }
    for (uint16_t i = 0u; i < MAX_CLIENTS; i++) {
        if (ipc.clients[i].isOpen) {
            ipc.clients[i].isOpen = false;
            MBP_ASSERT_NON_FATAL(CloseFd(&ipc.clients[i].fd), firstRet);
        }
    }
    MBP_ASSERT_NON_FATAL(CloseFd(&ipc.listenFd), firstRet);
    MBP_ASSERT_NON_FATAL(MbpIpcPath_Unlink(&ipc.path), firstRet);
    MBP_ASSERT_NON_FATAL(CloseFd(&ipc.stopFd), firstRet);
    return firstRet;
}
