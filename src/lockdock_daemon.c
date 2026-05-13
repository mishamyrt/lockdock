#include "lockdock_daemon.h"

#include "lockdock_display.h"
#include "lockdock_locker.h"
#include "lockdock_platform.h"
#include "lockdock_preferences.h"
#include "lockdock_runtime.h"

#include "lockdock_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_daemon_running = 1;
static _Atomic bool g_display_state_dirty = false;
static int g_daemon_wakeup_pipe[2] = {-1, -1};

static const useconds_t LOCKDOCK_DISPLAY_RECONFIGURATION_SETTLE_DELAY_US = 1500000;
static const int LOCKDOCK_DISPLAY_RELOCATION_RETRY_ATTEMPTS = 5;
static const useconds_t LOCKDOCK_DISPLAY_RELOCATION_RETRY_DELAY_US = 1000000;
static const time_t LOCKDOCK_DISPLAY_POLL_INTERVAL_SECONDS = 2;

typedef struct {
    CGDirectDisplayID dock_display;
    CGDirectDisplayID locked_display;
    bool has_dock_display;
    bool has_preferred_display;
    LockDockDisplayIdentity preferred_identity;
} LockDockStateSnapshot;

static void lockdock_signal_handler(int signal_number) {
    if (signal_number == SIGINT || signal_number == SIGTERM) {
        g_daemon_running = 0;
        if (g_daemon_wakeup_pipe[1] >= 0) {
            unsigned char token = 0;

            write(g_daemon_wakeup_pipe[1], &token, sizeof(token));
        }
    }
}

static void lockdock_notify_daemon(void) {
    unsigned char token = 0;
    ssize_t written;

    if (g_daemon_wakeup_pipe[1] < 0) {
        return;
    }

    do {
        written = write(g_daemon_wakeup_pipe[1], &token, sizeof(token));
    } while (written < 0 && errno == EINTR);
}

static void lockdock_mark_display_state_dirty(void) {
    lockdock_invalidate_display_name_cache();
    lockdock_invalidate_dock_orientation_cache();
    atomic_store(&g_display_state_dirty, true);
    lockdock_notify_daemon();
}

static void lockdock_display_reconfiguration_callback(
    CGDirectDisplayID display_id,
    CGDisplayChangeSummaryFlags flags,
    void *user_info) {
    (void)user_info;

    fprintf(stderr, "Display callback: display=%u flags=0x%x\n", display_id, flags);

    if ((flags & kCGDisplayBeginConfigurationFlag) != 0) {
        return;
    }

    lockdock_mark_display_state_dirty();
}

static void lockdock_set_error(char *buffer,
                               size_t buffer_size,
                               const char *message) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", message);
}

static bool lockdock_write_pid_file(const char *pid_path,
                                    char *error,
                                    size_t error_size) {
    char pid_text[32];
    size_t written = 0;
    int fd;
    int pid_text_length;

    if (pid_path == NULL || pid_path[0] == '\0') {
        lockdock_set_error(error, error_size, "Internal error");
        return false;
    }

    pid_text_length = snprintf(pid_text, sizeof(pid_text), "%ld\n", (long)getpid());
    if (pid_text_length < 0 || pid_text_length >= (int)sizeof(pid_text)) {
        lockdock_set_error(error, error_size, "Failed to format daemon PID");
        return false;
    }

    fd = open(pid_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        snprintf(error, error_size, "Failed to open daemon pid file '%s': %s",
                 pid_path, strerror(errno));
        return false;
    }

    while (written < (size_t)pid_text_length) {
        ssize_t nwritten =
            write(fd, pid_text + written, (size_t)pid_text_length - written);

        if (nwritten > 0) {
            written += (size_t)nwritten;
            continue;
        }

        if (nwritten < 0 && errno == EINTR) {
            continue;
        }

        snprintf(error, error_size, "Failed to write daemon pid file '%s': %s",
                 pid_path, strerror(nwritten < 0 ? errno : EIO));
        close(fd);
        unlink(pid_path);
        return false;
    }

    if (close(fd) != 0) {
        snprintf(error, error_size, "Failed to finalize daemon pid file '%s': %s",
                 pid_path, strerror(errno));
        unlink(pid_path);
        return false;
    }

    return true;
}

static void lockdock_result_response(bool success,
                                     const char *reason,
                                     char *buffer,
                                     size_t buffer_size) {
    LockDockIpcResult result;
    char error[LOCKDOCK_ERROR_BUFFER_SIZE];

    memset(&result, 0, sizeof(result));
    result.success = success;

    if (!success && reason != NULL) {
        snprintf(result.reason, sizeof(result.reason), "%s", reason);
    }

    if (!lockdock_ipc_serialize_result_response_json(&result, buffer, buffer_size,
                                                     error, sizeof(error))) {
        snprintf(buffer, buffer_size,
                 "{\"success\":false,\"reason\":\"Internal error\"}");
    }
}

static void lockdock_trim_request(char *request) {
    size_t length;

    if (request == NULL) {
        return;
    }

    length = strlen(request);
    while (length > 0 &&
           (request[length - 1] == '\n' || request[length - 1] == '\r' ||
            request[length - 1] == ' ' || request[length - 1] == '\t')) {
        request[--length] = '\0';
    }
}

static bool lockdock_build_state_response(char *buffer,
                                          size_t buffer_size,
                                          char *error,
                                          size_t error_size) {
    LockDockStatus status;
    LockDockIpcState state_response;
    CGDirectDisplayID target_display = lockdock_locker_get_target();
    int target_index = -1;

    if (!lockdock_query_status(&status, error, error_size)) {
        return false;
    }

    memset(&state_response, 0, sizeof(state_response));
    state_response.display_count = (uint8_t)status.display_count;
    state_response.location_index = status.location_index;

    if (target_display != 0) {
        target_index = lockdock_status_index_for_display(&status, target_display);
        if (target_index >= 0) {
            state_response.has_target = true;
            state_response.target_index = target_index;
        }
    }

    for (uint32_t i = 0; i < status.display_count; i++) {
        lockdock_copy_display_label(status.displays[i], state_response.displays[i],
                                    sizeof(state_response.displays[i]));
    }

    return lockdock_ipc_serialize_state_response_json(
        &state_response, buffer, buffer_size, error, error_size);
}

static bool lockdock_resolve_target_display(const LockDockStatus *status,
                                            int target_index,
                                            CGDirectDisplayID *display_id_out,
                                            char *error,
                                            size_t error_size) {
    if (status == NULL || display_id_out == NULL) {
        lockdock_set_error(error, error_size, "Internal error");
        return false;
    }

    if (target_index < 0 || target_index >= (int)status->display_count) {
        snprintf(error, error_size, "Display index %d is out of range",
                 target_index);
        return false;
    }

    *display_id_out = status->displays[target_index];
    return true;
}

static bool lockdock_copy_status_dock_display(CGDirectDisplayID *display_id_out,
                                              char *error,
                                              size_t error_size) {
    LockDockStatus status;

    if (display_id_out == NULL) {
        lockdock_set_error(error, error_size, "Internal error");
        return false;
    }

    if (!lockdock_query_status(&status, error, error_size)) {
        return false;
    }

    *display_id_out = status.displays[status.location_index];
    fprintf(stderr, "Status: dock_display=%u display_count=%u location_index=%d\n",
            *display_id_out, status.display_count, status.location_index);
    return true;
}

static bool lockdock_relocate_display_until_current(CGDirectDisplayID display_id,
                                                    char *error,
                                                    size_t error_size) {
    CGDirectDisplayID dock_display = 0;

    for (int attempt = 0; attempt < LOCKDOCK_DISPLAY_RELOCATION_RETRY_ATTEMPTS;
         attempt++) {
        fprintf(stderr, "Relocate attempt %d/%d: target_display=%u\n", attempt + 1,
                LOCKDOCK_DISPLAY_RELOCATION_RETRY_ATTEMPTS, display_id);

        if (!lockdock_relocate_display(display_id, error, error_size)) {
            fprintf(stderr, "Relocate attempt failed: target_display=%u error=%s\n",
                    display_id, error);
            return false;
        }

        usleep(LOCKDOCK_DISPLAY_RELOCATION_RETRY_DELAY_US);

        if (!lockdock_copy_status_dock_display(&dock_display, error, error_size)) {
            fprintf(stderr,
                    "Relocate verification failed: target_display=%u error=%s\n",
                    display_id, error);
            return false;
        }

        if (dock_display == display_id) {
            fprintf(stderr, "Relocate verified: dock_display=%u\n", dock_display);
            return true;
        }

        fprintf(stderr,
                "Relocate not verified: dock_display=%u expected=%u; retrying\n",
                dock_display, display_id);
    }

    snprintf(error, error_size, "Dock is on display %u (expected %u)", dock_display,
             display_id);
    return false;
}

static bool lockdock_reconcile_display_state(char *error, size_t error_size) {
    LockDockDisplayIdentity preferred_identity;
    CGDirectDisplayID locked_display = lockdock_locker_get_target();
    CGDirectDisplayID preferred_display = 0;
    LockDockStatus status;

    fprintf(stderr, "Display reconcile: locked_display=%u\n", locked_display);

    if (locked_display != 0 && lockdock_find_display_index(locked_display) < 0) {
        fprintf(stderr,
                "Display reconcile: locked display %u is inactive; clearing lock\n",
                locked_display);
        lockdock_locker_clear_target();
        locked_display = 0;
    }

    if (locked_display != 0) {
        if (!lockdock_query_status(&status, error, error_size)) {
            fprintf(stderr, "Display reconcile: status failed: %s\n", error);
            return false;
        }

        fprintf(stderr,
                "Display reconcile: active lock=%u dock_display=%u display_count=%u "
                "location_index=%d\n",
                locked_display, status.displays[status.location_index],
                status.display_count, status.location_index);

        if (status.displays[status.location_index] == locked_display) {
            fprintf(stderr,
                    "Display reconcile: Dock already matches locked display %u\n",
                    locked_display);
            return true;
        }

        return lockdock_relocate_display_until_current(locked_display, error,
                                                       error_size);
    }

    if (!lockdock_preferences_load_preferred_display(&preferred_identity)) {
        fprintf(stderr, "Display reconcile: no preferred display saved\n");
        return true;
    }

    if (!lockdock_find_active_display_by_identity(&preferred_identity,
                                                  &preferred_display)) {
        fprintf(stderr, "Display reconcile: preferred display is not active yet\n");
        return true;
    }

    fprintf(stderr,
            "Display reconcile: restoring preferred display %u as lock target\n",
            preferred_display);

    /* During display reconfiguration Dock window probing can be stale. */
    if (!lockdock_relocate_display_until_current(preferred_display, error,
                                                 error_size)) {
        return false;
    }

    fprintf(stderr, "Display reconcile: setting lock target to %u\n",
            preferred_display);
    return lockdock_locker_set_target(preferred_display, error, error_size);
}

static bool lockdock_capture_state_snapshot(LockDockStateSnapshot *snapshot,
                                            const LockDockStatus *status,
                                            bool capture_dock_display,
                                            char *error,
                                            size_t error_size) {
    LockDockStatus current_status;

    if (snapshot == NULL) {
        lockdock_set_error(error, error_size, "Internal error");
        return false;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->locked_display = lockdock_locker_get_target();
    snapshot->has_preferred_display =
        lockdock_preferences_load_preferred_display(&snapshot->preferred_identity);

    if (!capture_dock_display) {
        return true;
    }

    if (status == NULL) {
        if (!lockdock_query_status(&current_status, error, error_size)) {
            return false;
        }

        status = &current_status;
    }

    if (status->location_index < 0 ||
        status->location_index >= (int)status->display_count) {
        lockdock_set_error(error, error_size, "Internal error");
        return false;
    }

    snapshot->dock_display = status->displays[status->location_index];
    snapshot->has_dock_display = true;
    return true;
}

static bool lockdock_restore_preferred_display(const LockDockStateSnapshot *snapshot,
                                               char *error,
                                               size_t error_size) {
    if (snapshot == NULL) {
        lockdock_set_error(error, error_size, "Internal error");
        return false;
    }

    if (!snapshot->has_preferred_display) {
        return lockdock_preferences_clear_preferred_display(error, error_size);
    }

    return lockdock_preferences_save_preferred_display(&snapshot->preferred_identity,
                                                       error, error_size);
}

static bool lockdock_restore_runtime_state(const LockDockStateSnapshot *snapshot,
                                           bool restore_dock_display,
                                           char *error,
                                           size_t error_size) {
    char dock_error[LOCKDOCK_ERROR_BUFFER_SIZE] = "";
    char lock_error[LOCKDOCK_ERROR_BUFFER_SIZE] = "";
    bool dock_restored = true;
    bool lock_restored = true;

    if (snapshot == NULL) {
        lockdock_set_error(error, error_size, "Internal error");
        return false;
    }

    lockdock_locker_clear_target();

    if (restore_dock_display) {
        if (!snapshot->has_dock_display) {
            lockdock_set_error(dock_error, sizeof(dock_error), "Internal error");
            dock_restored = false;
        } else if (!lockdock_relocate_display(snapshot->dock_display, dock_error,
                                              sizeof(dock_error))) {
            dock_restored = false;
        }
    }

    if (snapshot->locked_display != 0 &&
        !lockdock_locker_set_target(snapshot->locked_display, lock_error,
                                    sizeof(lock_error))) {
        lock_restored = false;
    }

    if (dock_restored && lock_restored) {
        return true;
    }

    if (!dock_restored && !lock_restored) {
        snprintf(error, error_size, "%s; %s", dock_error, lock_error);
    } else if (!dock_restored) {
        lockdock_set_error(error, error_size, dock_error);
    } else {
        lockdock_set_error(error, error_size, lock_error);
    }

    return false;
}

static void lockdock_set_rollback_error(char *error,
                                        size_t error_size,
                                        const char *operation_error,
                                        const char *preference_rollback_error,
                                        const char *runtime_rollback_error) {
    bool has_preference_rollback_error =
        preference_rollback_error != NULL && preference_rollback_error[0] != '\0';
    bool has_runtime_rollback_error =
        runtime_rollback_error != NULL && runtime_rollback_error[0] != '\0';

    if (!has_preference_rollback_error && !has_runtime_rollback_error) {
        lockdock_set_error(error, error_size, operation_error);
        return;
    }

    if (has_preference_rollback_error && has_runtime_rollback_error) {
        snprintf(error, error_size,
                 "%s (rollback failed: preferences: %s; runtime: %s)",
                 operation_error, preference_rollback_error, runtime_rollback_error);
        return;
    }

    if (has_preference_rollback_error) {
        snprintf(error, error_size, "%s (rollback failed: preferences: %s)",
                 operation_error, preference_rollback_error);
        return;
    }

    snprintf(error, error_size, "%s (rollback failed: runtime: %s)", operation_error,
             runtime_rollback_error);
}

static bool lockdock_apply_unlock(char *error, size_t error_size) {
    LockDockStateSnapshot previous_state;
    char operation_error[LOCKDOCK_ERROR_BUFFER_SIZE];
    char preference_rollback_error[LOCKDOCK_ERROR_BUFFER_SIZE] = "";

    if (!lockdock_capture_state_snapshot(&previous_state, NULL, false, error,
                                         error_size)) {
        return false;
    }

    if (!lockdock_preferences_clear_preferred_display(operation_error,
                                                      sizeof(operation_error))) {
        if (!lockdock_restore_preferred_display(&previous_state,
                                                preference_rollback_error,
                                                sizeof(preference_rollback_error))) {
            lockdock_set_rollback_error(error, error_size, operation_error,
                                        preference_rollback_error, NULL);
        } else {
            lockdock_set_error(error, error_size, operation_error);
        }

        return false;
    }

    lockdock_locker_clear_target();
    return true;
}

static bool lockdock_apply_set_state(
    const LockDockStatus *status,
    CGDirectDisplayID display_id,
    const LockDockDisplayIdentity *preferred_identity,
    char *error,
    size_t error_size) {
    LockDockStateSnapshot previous_state;
    bool restore_dock_display = false;
    bool clear_existing_lock_for_relocation = false;
    char operation_error[LOCKDOCK_ERROR_BUFFER_SIZE];
    char preference_rollback_error[LOCKDOCK_ERROR_BUFFER_SIZE] = "";
    char runtime_rollback_error[LOCKDOCK_ERROR_BUFFER_SIZE] = "";

    if (preferred_identity == NULL) {
        lockdock_set_error(error, error_size, "Internal error");
        return false;
    }

    if (!lockdock_capture_state_snapshot(&previous_state, status, true, error,
                                         error_size)) {
        return false;
    }

    restore_dock_display =
        previous_state.has_dock_display && previous_state.dock_display != display_id;
    clear_existing_lock_for_relocation = restore_dock_display &&
                                         previous_state.locked_display != 0 &&
                                         previous_state.locked_display != display_id;

    if (clear_existing_lock_for_relocation) {
        /* The current lock blocks the synthetic edge approach used for relocation.
         */
        lockdock_locker_clear_target();
    }

    if (restore_dock_display &&
        !lockdock_relocate_display(display_id, operation_error,
                                   sizeof(operation_error))) {
        if (!lockdock_restore_runtime_state(&previous_state, restore_dock_display,
                                            runtime_rollback_error,
                                            sizeof(runtime_rollback_error))) {
            lockdock_set_rollback_error(error, error_size, operation_error, NULL,
                                        runtime_rollback_error);
        } else {
            lockdock_set_error(error, error_size, operation_error);
        }

        return false;
    }

    if (!lockdock_locker_set_target(display_id, operation_error,
                                    sizeof(operation_error))) {
        if (!lockdock_restore_runtime_state(&previous_state, restore_dock_display,
                                            runtime_rollback_error,
                                            sizeof(runtime_rollback_error))) {
            lockdock_set_rollback_error(error, error_size, operation_error, NULL,
                                        runtime_rollback_error);
        } else {
            lockdock_set_error(error, error_size, operation_error);
        }

        return false;
    }

    if (!lockdock_preferences_save_preferred_display(
            preferred_identity, operation_error, sizeof(operation_error))) {
        if (!lockdock_restore_preferred_display(&previous_state,
                                                preference_rollback_error,
                                                sizeof(preference_rollback_error))) {
            /* Keep rolling back runtime even if preferences rollback fails. */
        }

        if (!lockdock_restore_runtime_state(&previous_state, restore_dock_display,
                                            runtime_rollback_error,
                                            sizeof(runtime_rollback_error))) {
            lockdock_set_rollback_error(error, error_size, operation_error,
                                        preference_rollback_error,
                                        runtime_rollback_error);
        } else if (preference_rollback_error[0] != '\0') {
            lockdock_set_rollback_error(error, error_size, operation_error,
                                        preference_rollback_error, NULL);
        } else {
            lockdock_set_error(error, error_size, operation_error);
        }

        return false;
    }

    return true;
}

static void lockdock_handle_request(const LockDockIpcRequest *request,
                                    char *response,
                                    size_t response_size) {
    char error[LOCKDOCK_ERROR_BUFFER_SIZE];
    LockDockDisplayIdentity preferred_identity;
    LockDockStatus status;
    CGDirectDisplayID display_id = 0;

    if (request == NULL) {
        lockdock_result_response(false, "Internal error", response, response_size);
        return;
    }

    if (request->command == LOCKDOCK_IPC_COMMAND_GET_STATE) {
        if (!lockdock_build_state_response(response, response_size, error,
                                           sizeof(error))) {
            lockdock_result_response(false, error, response, response_size);
        }
        return;
    }

    if (request->command == LOCKDOCK_IPC_COMMAND_UNLOCK) {
        if (!lockdock_apply_unlock(error, sizeof(error))) {
            lockdock_result_response(false, error, response, response_size);
            return;
        }
        lockdock_result_response(true, NULL, response, response_size);
        return;
    }

    if (request->command != LOCKDOCK_IPC_COMMAND_SET_STATE) {
        lockdock_result_response(false, "Unknown command", response, response_size);
        return;
    }

    if (!lockdock_query_status(&status, error, sizeof(error))) {
        lockdock_result_response(false, error, response, response_size);
        return;
    }

    if (!lockdock_resolve_target_display(&status, request->target, &display_id,
                                         error, sizeof(error))) {
        lockdock_result_response(false, error, response, response_size);
        return;
    }

    if (!lockdock_copy_display_identity(display_id, &preferred_identity)) {
        lockdock_result_response(false, "Could not identify target display",
                                 response, response_size);
        return;
    }

    if (!lockdock_apply_set_state(&status, display_id, &preferred_identity, error,
                                  sizeof(error))) {
        lockdock_result_response(false, error, response, response_size);
        return;
    }

    lockdock_result_response(true, NULL, response, response_size);
}

static bool lockdock_read_request(int fd,
                                  char *buffer,
                                  size_t buffer_size,
                                  char *error,
                                  size_t error_size) {
    size_t used = 0;

    while (used + 1 < buffer_size) {
        ssize_t nread = read(fd, buffer + used, buffer_size - used - 1);

        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }

            snprintf(error, error_size, "Failed to read client request: %s",
                     strerror(errno));
            return false;
        }

        if (nread == 0) {
            break;
        }

        used += (size_t)nread;
    }

    if (used == 0) {
        snprintf(error, error_size, "Client sent an empty request");
        return false;
    }

    if (used + 1 >= buffer_size) {
        snprintf(error, error_size, "Client request exceeded buffer");
        return false;
    }

    buffer[used] = '\0';
    lockdock_trim_request(buffer);
    if (buffer[0] == '\0') {
        snprintf(error, error_size, "Client sent an empty request");
        return false;
    }

    return true;
}

static bool lockdock_write_response(int fd,
                                    const char *response,
                                    char *error,
                                    size_t error_size) {
    size_t length = strlen(response);
    size_t offset = 0;

    while (offset < length) {
        ssize_t written = write(fd, response + offset, length - offset);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }

            snprintf(error, error_size, "Failed to write client response: %s",
                     strerror(errno));
            return false;
        }

        offset += (size_t)written;
    }

    return true;
}

static int lockdock_probe_existing_socket(const char *socket_path,
                                          char *error,
                                          size_t error_size) {
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) {
        snprintf(error, error_size, "Failed to create probe socket: %s",
                 strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_len = sizeof(addr);
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        close(fd);
        snprintf(error, error_size, "lockdock daemon is already running");
        return 1;
    }

    if (errno != ENOENT && errno != ECONNREFUSED) {
        snprintf(error, error_size, "Failed to probe daemon socket: %s",
                 strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

#define LOCKDOCK_SOCKET_BACKLOG 8

static int lockdock_open_server_socket(char *socket_path,
                                       size_t socket_path_size,
                                       char *error,
                                       size_t error_size) {
    struct sockaddr_un addr;
    int fd = -1;
    int probe_result;

    if (!lockdock_ipc_ensure_socket_dir(error, error_size) ||
        !lockdock_ipc_copy_socket_path(socket_path, socket_path_size, error,
                                       error_size)) {
        return -1;
    }

    probe_result = lockdock_probe_existing_socket(socket_path, error, error_size);
    if (probe_result != 0) {
        return probe_result > 0 ? -2 : -1;
    }

    unlink(socket_path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(error, error_size, "Failed to create daemon socket: %s",
                 strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_len = sizeof(addr);
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(error, error_size, "Failed to bind daemon socket '%s': %s",
                 socket_path, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, LOCKDOCK_SOCKET_BACKLOG) != 0) {
        snprintf(error, error_size, "Failed to listen on daemon socket: %s",
                 strerror(errno));
        close(fd);
        unlink(socket_path);
        return -1;
    }

    return fd;
}

static bool lockdock_register_display_callback(char *error, size_t error_size) {
    CGError cg_error = CGDisplayRegisterReconfigurationCallback(
        lockdock_display_reconfiguration_callback, NULL);

    if (cg_error == kCGErrorSuccess) {
        return true;
    }

    snprintf(error, error_size,
             "Failed to register display reconfiguration callback (%d)",
             (int)cg_error);
    return false;
}

static void lockdock_remove_display_callback(void) {
    CGDisplayRemoveReconfigurationCallback(lockdock_display_reconfiguration_callback,
                                           NULL);
}

static void lockdock_close_wakeup_pipe(void) {
    if (g_daemon_wakeup_pipe[0] >= 0) {
        close(g_daemon_wakeup_pipe[0]);
        g_daemon_wakeup_pipe[0] = -1;
    }

    if (g_daemon_wakeup_pipe[1] >= 0) {
        close(g_daemon_wakeup_pipe[1]);
        g_daemon_wakeup_pipe[1] = -1;
    }
}

static bool lockdock_make_fd_nonblocking(int fd, char *error, size_t error_size) {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        snprintf(error, error_size, "Failed to read fd flags: %s", strerror(errno));
        return false;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        snprintf(error, error_size, "Failed to set non-blocking fd: %s",
                 strerror(errno));
        return false;
    }

    return true;
}

static bool lockdock_open_wakeup_pipe(char *error, size_t error_size) {
    if (pipe(g_daemon_wakeup_pipe) != 0) {
        snprintf(error, error_size, "Failed to create daemon wakeup pipe: %s",
                 strerror(errno));
        return false;
    }

    if (!lockdock_make_fd_nonblocking(g_daemon_wakeup_pipe[0], error, error_size) ||
        !lockdock_make_fd_nonblocking(g_daemon_wakeup_pipe[1], error, error_size)) {
        lockdock_close_wakeup_pipe();
        return false;
    }

    return true;
}

#define LOCKDOCK_DRAIN_WAKEUP_PIPE_BUFFER 64

static void lockdock_drain_wakeup_pipe(void) {
    char buffer[LOCKDOCK_DRAIN_WAKEUP_PIPE_BUFFER];

    if (g_daemon_wakeup_pipe[0] < 0) {
        return;
    }

    while (true) {
        ssize_t nread = read(g_daemon_wakeup_pipe[0], buffer, sizeof(buffer));

        if (nread > 0) {
            continue;
        }

        if (nread < 0 && errno == EINTR) {
            continue;
        }

        break;
    }
}

static void lockdock_reconcile_pending_display_state(char *error,
                                                     size_t error_size) {
    if (!atomic_exchange(&g_display_state_dirty, false)) {
        return;
    }

    fprintf(stderr, "Display reconcile pending: waiting %.2fs for reconfiguration\n",
            (double)LOCKDOCK_DISPLAY_RECONFIGURATION_SETTLE_DELAY_US / 1000000.0);
    usleep(LOCKDOCK_DISPLAY_RECONFIGURATION_SETTLE_DELAY_US);

    if (!lockdock_reconcile_display_state(error, error_size)) {
        fprintf(stderr, "Display reconcile failed: %s\n", error);
    }
}

static void lockdock_poll_display_state(char *error, size_t error_size) {
    fprintf(stderr, "Display poll: checking display state\n");
    if (!lockdock_reconcile_display_state(error, error_size)) {
        fprintf(stderr, "Display poll reconcile failed: %s\n", error);
    }
}

int lockdock_run_daemon(void) {
    char socket_path[PATH_MAX] = {0};
    char pid_path[PATH_MAX] = {0};
    char error[LOCKDOCK_ERROR_BUFFER_SIZE];
    int listen_fd = -1;
    bool display_callback_registered = false;
    int exit_code = 1;

    signal(SIGINT, lockdock_signal_handler);
    signal(SIGTERM, lockdock_signal_handler);
    signal(SIGPIPE, SIG_IGN);

    if (!lockdock_open_wakeup_pipe(error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        goto cleanup;
    }

    listen_fd = lockdock_open_server_socket(socket_path, sizeof(socket_path), error,
                                            sizeof(error));
    if (listen_fd < 0) {
        fprintf(stderr, "%s\n", error);
        goto cleanup;
    }

    if (!lockdock_register_display_callback(error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        goto cleanup;
    }
    display_callback_registered = true;

    if (!lockdock_reconcile_display_state(error, sizeof(error))) {
        fprintf(stderr, "Display reconcile failed: %s\n", error);
    }

    if (!lockdock_ipc_copy_pid_path(pid_path, sizeof(pid_path), error,
                                    sizeof(error)) ||
        !lockdock_write_pid_file(pid_path, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        goto cleanup;
    }

    printf("lockdock daemon listening on %s\n", socket_path);

    while (g_daemon_running) {
        fd_set read_fds;
        struct timeval timeout;
        int select_result;
        int max_fd = listen_fd;

        if (g_daemon_wakeup_pipe[0] > max_fd) {
            max_fd = g_daemon_wakeup_pipe[0];
        }

        FD_ZERO(&read_fds);
        FD_SET(listen_fd, &read_fds);
        FD_SET(g_daemon_wakeup_pipe[0], &read_fds);
        timeout.tv_sec = LOCKDOCK_DISPLAY_POLL_INTERVAL_SECONDS;
        timeout.tv_usec = 0;

        select_result = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (select_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            fprintf(stderr, "Daemon select failed: %s\n", strerror(errno));
            break;
        }

        if (select_result == 0) {
            lockdock_poll_display_state(error, sizeof(error));
            continue;
        }

        if (FD_ISSET(g_daemon_wakeup_pipe[0], &read_fds)) {
            lockdock_drain_wakeup_pipe();
        }

        if (!g_daemon_running) {
            break;
        }

        lockdock_reconcile_pending_display_state(error, sizeof(error));

        if (select_result > 0 && FD_ISSET(listen_fd, &read_fds)) {
            int client_fd = accept(listen_fd, NULL, NULL);

            if (client_fd >= 0) {
                char request[LOCKDOCK_IPC_MAX_MESSAGE];
                char response[LOCKDOCK_IPC_MAX_MESSAGE];

                if (!lockdock_read_request(client_fd, request, sizeof(request),
                                           error, sizeof(error))) {
                    lockdock_result_response(false, error, response,
                                             sizeof(response));
                } else {
                    LockDockIpcRequest parsed_request;

                    if (!lockdock_ipc_parse_request_json(request, &parsed_request,
                                                         error, sizeof(error))) {
                        lockdock_result_response(false, error, response,
                                                 sizeof(response));
                    } else {
                        lockdock_handle_request(&parsed_request, response,
                                                sizeof(response));
                    }
                }

                if (!lockdock_write_response(client_fd, response, error,
                                             sizeof(error))) {
                    fprintf(stderr, "%s\n", error);
                }

                close(client_fd);
            }
        }

        lockdock_reconcile_pending_display_state(error, sizeof(error));
    }

    exit_code = 0;

cleanup:
    if (display_callback_registered) {
        lockdock_remove_display_callback();
    }
    lockdock_locker_shutdown();
    lockdock_close_wakeup_pipe();
    if (listen_fd >= 0) {
        close(listen_fd);
    }
    if (socket_path[0] != '\0') {
        unlink(socket_path);
    }
    if (pid_path[0] != '\0') {
        unlink(pid_path);
    }
    return exit_code;
}
