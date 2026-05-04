#include "lockdockd_commands.h"

#include <stdio.h>
#include <string.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

int main(int argc, char **argv) {
    const char *command;

    if (argc < 2) {
        lockdockd_print_usage(argv[0]);
        return 0;
    }

    command = argv[1];

    if (strcmp(command, "list") == 0) {
        return lockdockd_cmd_list();
    }

    if (strcmp(command, "relocate") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s relocate <display-id>\n", argv[0]);
            return 1;
        }

        return lockdockd_cmd_relocate(argv[2]);
    }

    if (strcmp(command, "lock") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s lock <display-id>\n", argv[0]);
            return 1;
        }

        return lockdockd_cmd_lock(argv[2]);
    }

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
