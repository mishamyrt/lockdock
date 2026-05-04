#include "lockdockd_commands.h"

#include "lockdockd_daemon.h"
#include "lockdockd_ipc.h"
#include "lockdockd_runtime.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int lockdockd_print_daemon_response(const char *request) {
    char response[LOCKDOCKD_IPC_MAX_MESSAGE];
    char error[LOCKDOCKD_ERROR_BUFFER_SIZE];

    if (lockdockd_ipc_send_request(request, response, sizeof(response), error,
                                   sizeof(error)) != 0) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }

    puts(response);
    if (lockdockd_ipc_response_is_error(response)) {
        return 1;
    }

    return 0;
}

int lockdockd_cmd_daemon(void) {
    return lockdockd_run_daemon();
}

int lockdockd_cmd_status(void) {
    return lockdockd_print_daemon_response("status");
}

int lockdockd_cmd_list(void) {
    char response[LOCKDOCKD_IPC_MAX_MESSAGE];
    char error[LOCKDOCKD_ERROR_BUFFER_SIZE];
    LockDockdStatus local_status;
    bool has_target = false;
    int target_index = -1;

    if (lockdockd_ipc_send_request("status", response, sizeof(response), error,
                                   sizeof(error)) != 0) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }

    if (!lockdockd_ipc_parse_status_indices(response, &local_status.location_index,
                                            &has_target, &target_index, error,
                                            sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }

    memset(local_status.displays, 0, sizeof(local_status.displays));
    local_status.display_count =
        lockdockd_get_active_displays(local_status.displays, LOCKDOCKD_MAX_DISPLAYS);

    for (uint32_t i = 0; i < local_status.display_count; i++) {
        char display_name[LOCKDOCKD_DISPLAY_NAME_BUFFER_SIZE];

        lockdockd_copy_display_label(local_status.displays[i], display_name,
                                     sizeof(display_name));

        printf("[%u] %s", i, display_name);
        if ((int)i == local_status.location_index) {
            printf(" *");
        }
        if (has_target && (int)i == target_index) {
            printf(" locked");
        }
        printf("\n");
    }

    return 0;
}

int lockdockd_cmd_relocate(const char *display_arg) {
    char request[LOCKDOCKD_IPC_MAX_MESSAGE];

    if (snprintf(request, sizeof(request), "relocate %s", display_arg) >=
        (int)sizeof(request)) {
        fprintf(stderr, "Display argument is too long\n");
        return 1;
    }

    return lockdockd_print_daemon_response(request);
}

int lockdockd_cmd_lock(const char *display_arg) {
    char request[LOCKDOCKD_IPC_MAX_MESSAGE];

    if (snprintf(request, sizeof(request), "lock %s", display_arg) >=
        (int)sizeof(request)) {
        fprintf(stderr, "Display argument is too long\n");
        return 1;
    }

    return lockdockd_print_daemon_response(request);
}

int lockdockd_cmd_unlock(void) {
    return lockdockd_print_daemon_response("unlock");
}

void lockdockd_print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s                          Start the foreground daemon\n", prog);
    printf("  %s status                   Print daemon status JSON\n", prog);
    printf("  %s list                     List all displays via daemon status\n",
           prog);
    printf("  %s relocate <display-id>    Move Dock to a display\n", prog);
    printf("  %s lock <display-id>        Lock Dock to a display\n", prog);
    printf("  %s unlock                   Clear the current Dock lock\n", prog);
}
