#include "lockdockd_commands.h"

#include "lockdockd_daemon.h"
#include "lockdockd_launchagent.h"

#include <stdio.h>

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
