#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mbp.h"

static MbpStatus ParseArgs(int argc, char *argv[], const char **configPath, const char **socketPath);
static MbpStatus BlockStopSignals(sigset_t *stopSignals);
static MbpStatus WaitForStop(const sigset_t *stopSignals);

static MbpStatus ParseArgs(int argc, char *argv[], const char **configPath, const char **socketPath) {
    bool isUsageError = false;
    int option = getopt(argc, argv, "c:s:");
    while (option != -1) {
        switch (option) {
        case 'c':
            *configPath = optarg;
            break;
        case 's':
            *socketPath = optarg;
            break;
        default:
            isUsageError = true;
            break;
        }
        option = getopt(argc, argv, "c:s:");
    }
    if (isUsageError || (optind != argc)) {
        (void) fprintf(stderr, "<3>usage: %s [-c config] [-s socket]\n", argv[0]);
        return MbpStatusParam;
    }
    return MbpStatusOk;
}

static MbpStatus BlockStopSignals(sigset_t *stopSignals) {
    struct sigaction ignore;
    (void) memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    if ((sigemptyset(stopSignals) != 0) || (sigaddset(stopSignals, SIGINT) != 0) || (sigaddset(stopSignals, SIGTERM) != 0)) {
        return MbpStatusOs;
    }
    if (pthread_sigmask(SIG_BLOCK, stopSignals, NULL) != 0) {
        return MbpStatusOs;
    }
    if (sigaction(SIGPIPE, &ignore, NULL) != 0) {
        return MbpStatusOs;
    }
    return MbpStatusOk;
}

static MbpStatus WaitForStop(const sigset_t *stopSignals) {
    int signalNumber = 0;
    const int waitRet = sigwait(stopSignals, &signalNumber);
    if (waitRet != 0) {
        (void) fprintf(stderr, "<3>sigwait failed: %s\n", strerror(waitRet));
        return MbpStatusOs;
    }
    return MbpStatusOk;
}

int main(int argc, char *argv[]) {
    const char *configPath = MBP_DEFAULT_CONFIG_PATH;
    const char *socketPath = MBP_DEFAULT_SOCKET_PATH;
    sigset_t stopSignals;

    if (ParseArgs(argc, argv, &configPath, &socketPath) != MbpStatusOk) {
        return EXIT_FAILURE;
    }
    if (BlockStopSignals(&stopSignals) != MbpStatusOk) {
        (void) fprintf(stderr, "<3>signal setup failed\n");
        return EXIT_FAILURE;
    }
    if (Mbp_Init(configPath, socketPath) != MbpStatusOk) {
        return EXIT_FAILURE;
    }
    const MbpStatus waitRet = WaitForStop(&stopSignals);
    const MbpStatus deinitRet = Mbp_Deinit();
    if ((waitRet != MbpStatusOk) || (deinitRet != MbpStatusOk)) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
