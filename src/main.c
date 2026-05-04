#include "lockdockd_commands.h"

#include <stdio.h>
#include <string.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

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

    if (strcmp(command, "version") == 0) {
        printf("lockdockd %s\n", APP_VERSION);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", command);
    lockdockd_print_usage(argv[0]);
    return 1;
}
