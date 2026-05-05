#include "lockdockd_daemon.h"

#include <stdio.h>
#include <string.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static void print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s                          Start the foreground daemon\n", prog);
    printf("  %s help | -h | --help       Show help\n", prog);
    printf("  %s version                  Show version\n", prog);
}

int main(int argc, char **argv) {
    const char *command;

    if (argc < 2) {
        return lockdockd_run_daemon();
    }

    command = argv[1];

    if (strcmp(command, "help") == 0 || strcmp(command, "-h") == 0 ||
        strcmp(command, "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    if (strcmp(command, "version") == 0) {
        printf("lockdockd %s\n", APP_VERSION);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", command);
    print_usage(argv[0]);
    return 1;
}
