#include "lockdockd_commands.h"

#include "lockdockd_daemon.h"

#include <stdio.h>

int lockdockd_cmd_daemon(void) {
    return lockdockd_run_daemon();
}

void lockdockd_print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s                          Start the foreground daemon\n", prog);
    printf("  %s help | -h | --help       Show help\n", prog);
    printf("  %s version                  Show version\n", prog);
}
