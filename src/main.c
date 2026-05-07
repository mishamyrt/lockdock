#include "lockdock_daemon.h"
#include "lockdock_ipc.h"
#include "lockdock_launchagent.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static int handle_enable(void) {
    char message[LOCKDOCK_LAUNCHAGENT_MESSAGE_SIZE];

    if (!lockdock_launchagent_enable(message, sizeof(message))) {
        fprintf(stderr, "%s\n", message);
        return 1;
    }

    printf("%s\n", message);
    return 0;
}

static int handle_disable(void) {
    char message[LOCKDOCK_LAUNCHAGENT_MESSAGE_SIZE];

    if (!lockdock_launchagent_disable(message, sizeof(message))) {
        fprintf(stderr, "%s\n", message);
        return 1;
    }

    printf("%s\n", message);
    return 0;
}

static int handle_list(void) {
    LockDockIpcState state;
    char error[LOCKDOCK_IPC_MAX_ERROR];

    if (!lockdock_ipc_get_state(&state, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }

    for (uint32_t i = 0; i < state.display_count; i++) {
        bool is_current = (int)i == state.location_index;
        bool is_locked = state.has_target && (int)i == state.target_index;

        printf("%u %s", i, state.displays[i]);
        if (is_current && is_locked) {
            printf(" [current, locked]");
        } else if (is_current) {
            printf(" [current]");
        } else if (is_locked) {
            printf(" [locked]");
        }
        printf("\n");
    }

    return 0;
}

static bool parse_display_index(const char *text, int *index_out) {
    char *endptr = NULL;
    long long parsed;

    if (text == NULL || index_out == NULL) {
        return false;
    }

    parsed = strtoll(text, &endptr, 10);
    if (endptr == text || *endptr != '\0' || parsed < 0 || parsed > INT_MAX) {
        return false;
    }

    *index_out = (int)parsed;
    return true;
}

static int handle_lock(const char *index_text) {
    char error[LOCKDOCK_IPC_MAX_ERROR];
    int index = 0;

    if (!parse_display_index(index_text, &index)) {
        fprintf(stderr, "Display index must be a non-negative integer\n");
        return 1;
    }

    if (!lockdock_ipc_lock(index, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }

    printf("Locked Dock to display %d\n", index);
    return 0;
}

static int handle_unlock(void) {
    char error[LOCKDOCK_IPC_MAX_ERROR];

    if (!lockdock_ipc_unlock(error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }

    printf("Unlocked Dock\n");
    return 0;
}

static void print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s run                      Run daemon in foreground\n", prog);
    printf("  %s enable                   Install and start the LaunchAgent\n",
           prog);
    printf("  %s disable                  Stop and remove the LaunchAgent\n", prog);
    printf("  %s list                     List displays and Dock state\n", prog);
    printf("  %s lock <index>             Lock the Dock to the display index\n",
           prog);
    printf("  %s unlock                   Unlock the Dock\n", prog);
    printf("  %s help                     Show help\n", prog);
    printf("  %s version                  Show version\n", prog);
}

int main(int argc, char **argv) {
    const char *command;

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    command = argv[1];

    if (strcmp(command, "run") == 0) {
        return lockdock_run_daemon();
    }

    if (strcmp(command, "enable") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Usage: %s enable\n", argv[0]);
            return 1;
        }
        return handle_enable();
    }

    if (strcmp(command, "disable") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Usage: %s disable\n", argv[0]);
            return 1;
        }
        return handle_disable();
    }

    if (strcmp(command, "list") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Usage: %s list\n", argv[0]);
            return 1;
        }
        return handle_list();
    }

    if (strcmp(command, "lock") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s lock <index>\n", argv[0]);
            return 1;
        }
        return handle_lock(argv[2]);
    }

    if (strcmp(command, "unlock") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Usage: %s unlock\n", argv[0]);
            return 1;
        }
        return handle_unlock();
    }

    if (strcmp(command, "help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    if (strcmp(command, "version") == 0) {
        printf("lockdock %s\n", APP_VERSION);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", command);
    print_usage(argv[0]);
    return 1;
}
