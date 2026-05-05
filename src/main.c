#include "lockdockd_daemon.h"
#include "lockdockd_launchagent.h"

#include <stdio.h>
#include <string.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif


int lockdockd_cmd_daemon(void) {
    return lockdockd_run_daemon();
}

int lockdockd_cmd_enable(void) {
    char message[LOCKDOCKD_LAUNCHAGENT_MESSAGE_SIZE];

    if (!lockdockd_launchagent_enable(message, sizeof(message))) {
        fprintf(stderr, "%s\n", message);
        return 1;
    }

    printf("%s\n", message);
    return 0;
}

int lockdockd_cmd_disable(void) {
    char message[LOCKDOCKD_LAUNCHAGENT_MESSAGE_SIZE];

    if (!lockdockd_launchagent_disable(message, sizeof(message))) {
        fprintf(stderr, "%s\n", message);
        return 1;
    }

    printf("%s\n", message);
    return 0;
}

void lockdockd_print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s                          Start the foreground daemon\n", prog);
    printf("  %s enable                   Install and start the LaunchAgent\n",
           prog);
    printf("  %s disable                  Stop and remove the LaunchAgent\n", prog);
    printf("  %s help | -h | --help       Show help\n", prog);
    printf("  %s version                  Show version\n", prog);
}

int main(int argc, char **argv) {
    const char *command;

    if (argc < 2) {
        return lockdockd_cmd_daemon();
    }

    command = argv[1];

    if (strcmp(command, "help") == 0 || strcmp(command, "-h") == 0 ||
        strcmp(command, "--help") == 0) {
        lockdockd_print_usage(argv[0]);
        return 0;
    }

    if (strcmp(command, "enable") == 0) {
        return lockdockd_cmd_enable();
    }

    if (strcmp(command, "disable") == 0) {
        return lockdockd_cmd_disable();
    }

    if (strcmp(command, "version") == 0) {
        printf("lockdockd %s\n", APP_VERSION);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", command);
    lockdockd_print_usage(argv[0]);
    return 1;
}
