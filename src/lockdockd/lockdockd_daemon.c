#include "lockdockd_daemon.h"

#include "lockdockd_ipc.h"
#include "lockdockd_locker.h"
#include "lockdockd_platform.h"
#include "lockdockd_preferences.h"
#include "lockdockd_request.h"
#include "lockdockd_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static volatile sig_atomic_t g_daemon_running = 1;
static _Atomic bool g_display_state_dirty = false;
static int g_daemon_wakeup_pipe[2] = {-1, -1};

typedef struct {
    CGDirectDisplayID dock_display;
    CGDirectDisplayID locked_display;
    bool has_dock_display;
    bool has_preferred_display;
    LockDockdDisplayIdentity preferred_identity;
} LockDockdStateSnapshot;

static void lockdockd_signal_handler(int signal_number) {
    if (signal_number == SIGINT || signal_number == SIGTERM) {
        g_daemon_running = 0;
        if (g_daemon_wakeup_pipe[1] >= 0) {
            unsigned char token = 0;

            write(g_daemon_wakeup_pipe[1], &token, sizeof(token));
        }
    }
}

static void lockdockd_notify_daemon(void) {
    unsigned char token = 0;
    ssize_t written;

    if (g_daemon_wakeup_pipe[1] < 0) {
        return;
    }

    do {
        written = write(g_daemon_wakeup_pipe[1], &token, sizeof(token));
    } while (written < 0 && errno == EINTR);
}

static void lockdockd_mark_display_state_dirty(void) {
    lockdockd_invalidate_display_name_cache();
    lockdockd_invalidate_dock_orientation_cache();
    atomic_store(&g_display_state_dirty, true);
    lockdockd_notify_daemon();
}

static void lockdockd_display_reconfiguration_callback(
    CGDirectDisplayID display_id,
    CGDisplayChangeSummaryFlags flags,
    void *user_info) {
    (void)display_id;
    (void)user_info;

    if ((flags & kCGDisplayBeginConfigurationFlag) != 0) {
        return;
    }

    lockdockd_mark_display_state_dirty();
}

static void lockdockd_set_error(char *buffer,
                                size_t buffer_size,
                                const char *message) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", message);
}

static bool lockdockd_append_bytes(char *buffer,
                                   size_t buffer_size,
                                   size_t *used,
                                   const char *data,
                                   size_t data_size) {
    if (*used + data_size >= buffer_size) {
        return false;
    }

    memcpy(buffer + *used, data, data_size);
    *used += data_size;
    buffer[*used] = '\0';
    return true;
}

static bool lockdockd_append_format(char *buffer,
                                    size_t buffer_size,
                                    size_t *used,
                                    const char *format,
                                    ...) {
    va_list args;
    int written;

    va_start(args, format);
    written = vsnprintf(buffer + *used, buffer_size - *used, format, args);
    va_end(args);

    if (written < 0 || *used + (size_t)written >= buffer_size) {
        return false;
    }

    *used += (size_t)written;
    return true;
}

static bool lockdockd_append_json_string(char *buffer,
                                         size_t buffer_size,
                                         size_t *used,
                                         const char *text) {
    const unsigned char *cursor = (const unsigned char *)text;

    if (!lockdockd_append_bytes(buffer, buffer_size, used, "\"", 1)) {
        return false;
    }

    while (cursor != NULL && *cursor != '\0') {
        char escaped[8];
        size_t escaped_size = 0;

        if (*cursor == '"' || *cursor == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)*cursor;
            escaped_size = 2;
        } else if (*cursor == '\n') {
            escaped[0] = '\\';
            escaped[1] = 'n';
            escaped_size = 2;
        } else if (*cursor == '\r') {
            escaped[0] = '\\';
            escaped[1] = 'r';
            escaped_size = 2;
        } else if (*cursor == '\t') {
            escaped[0] = '\\';
            escaped[1] = 't';
            escaped_size = 2;
        } else if (*cursor < 0x20) {
            snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
            escaped_size = strlen(escaped);
        } else {
            escaped[0] = (char)*cursor;
            escaped_size = 1;
        }

        if (!lockdockd_append_bytes(buffer, buffer_size, used, escaped,
                                    escaped_size)) {
            return false;
        }

        cursor++;
    }

    return lockdockd_append_bytes(buffer, buffer_size, used, "\"", 1);
}

static void lockdockd_json_error_response(char *buffer,
                                          size_t buffer_size,
                                          const char *message) {
    size_t used = 0;

    if (!lockdockd_append_bytes(buffer, buffer_size, &used,
                                "{\"success\":false,\"reason\":",
                                strlen("{\"success\":false,\"reason\":")) ||
        !lockdockd_append_json_string(buffer, buffer_size, &used, message) ||
        !lockdockd_append_bytes(buffer, buffer_size, &used, "}", 1)) {
        snprintf(buffer, buffer_size,
                 "{\"success\":false,\"reason\":\"Internal "
                 "error\"}");
    }
}

static void lockdockd_success_response(char *buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size, "{\"success\":true}");
}

static void lockdockd_trim_request(char *request) {
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

static bool lockdockd_build_state_response(char *buffer,
                                           size_t buffer_size,
                                           char *error,
                                           size_t error_size) {
    LockDockdStatus status;
    size_t used = 0;
    CGDirectDisplayID target_display = lockdockd_locker_get_target();
    int target_index = -1;

    if (!lockdockd_query_status(&status, error, error_size)) {
        return false;
    }

    if (target_display != 0) {
        target_index = lockdockd_status_index_for_display(&status, target_display);
    }

    if (!lockdockd_append_bytes(buffer, buffer_size, &used, "{\"displays\":[", 13)) {
        snprintf(error, error_size, "State response buffer is too small");
        return false;
    }

    for (uint32_t i = 0; i < status.display_count; i++) {
        char display_name[LOCKDOCKD_DISPLAY_NAME_BUFFER_SIZE];

        lockdockd_copy_display_label(status.displays[i], display_name,
                                     sizeof(display_name));

        if (i > 0 && !lockdockd_append_bytes(buffer, buffer_size, &used, ",", 1)) {
            snprintf(error, error_size, "State response buffer is too small");
            return false;
        }

        if (!lockdockd_append_json_string(buffer, buffer_size, &used,
                                          display_name)) {
            snprintf(error, error_size, "State response buffer is too small");
            return false;
        }
    }

    if (!lockdockd_append_format(buffer, buffer_size, &used, "],\"location\":%d",
                                 status.location_index)) {
        snprintf(error, error_size, "State response buffer is too small");
        return false;
    }

    if (target_index >= 0 &&
        !lockdockd_append_format(buffer, buffer_size, &used, ",\"target\":%d",
                                 target_index)) {
        snprintf(error, error_size, "State response buffer is too small");
        return false;
    }

    if (!lockdockd_append_bytes(buffer, buffer_size, &used, "}", 1)) {
        snprintf(error, error_size, "State response buffer is too small");
        return false;
    }

    return true;
}

static bool lockdockd_resolve_target_display(const LockDockdStatus *status,
                                             int target_index,
                                             CGDirectDisplayID *display_id_out,
                                             char *error,
                                             size_t error_size) {
    if (status == NULL || display_id_out == NULL) {
        lockdockd_set_error(error, error_size, "Internal error");
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

static bool lockdockd_reconcile_display_state(char *error, size_t error_size) {
    LockDockdDisplayIdentity preferred_identity;
    CGDirectDisplayID locked_display = lockdockd_locker_get_target();
    CGDirectDisplayID preferred_display = 0;
    LockDockdStatus status;

    if (locked_display != 0 && lockdockd_find_display_index(locked_display) < 0) {
        lockdockd_locker_clear_target();
        locked_display = 0;
    }

    if (!lockdockd_preferences_load_preferred_display(&preferred_identity)) {
        return true;
    }

    if (locked_display != 0) {
        return true;
    }

    if (!lockdockd_find_active_display_by_identity(&preferred_identity,
                                                   &preferred_display)) {
        return true;
    }

    if (!lockdockd_query_status(&status, error, error_size)) {
        return false;
    }

    if (status.displays[status.location_index] == preferred_display) {
        return lockdockd_locker_set_target(preferred_display, error, error_size);
    }

    if (!lockdockd_relocate_display(preferred_display, error, error_size)) {
        return false;
    }

    return lockdockd_locker_set_target(preferred_display, error, error_size);
}

static bool lockdockd_capture_state_snapshot(LockDockdStateSnapshot *snapshot,
                                             const LockDockdStatus *status,
                                             bool capture_dock_display,
                                             char *error,
                                             size_t error_size) {
    LockDockdStatus current_status;

    if (snapshot == NULL) {
        lockdockd_set_error(error, error_size, "Internal error");
        return false;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->locked_display = lockdockd_locker_get_target();
    snapshot->has_preferred_display =
        lockdockd_preferences_load_preferred_display(&snapshot->preferred_identity);

    if (!capture_dock_display) {
        return true;
    }

    if (status == NULL) {
        if (!lockdockd_query_status(&current_status, error, error_size)) {
            return false;
        }

        status = &current_status;
    }

    if (status->location_index < 0 ||
        status->location_index >= (int)status->display_count) {
        lockdockd_set_error(error, error_size, "Internal error");
        return false;
    }

    snapshot->dock_display = status->displays[status->location_index];
    snapshot->has_dock_display = true;
    return true;
}

static bool lockdockd_restore_preferred_display(
    const LockDockdStateSnapshot *snapshot,
    char *error,
    size_t error_size) {
    if (snapshot == NULL) {
        lockdockd_set_error(error, error_size, "Internal error");
        return false;
    }

    if (!snapshot->has_preferred_display) {
        return lockdockd_preferences_clear_preferred_display(error, error_size);
    }

    return lockdockd_preferences_save_preferred_display(
        &snapshot->preferred_identity, error, error_size);
}

static bool lockdockd_restore_runtime_state(const LockDockdStateSnapshot *snapshot,
                                            bool restore_dock_display,
                                            char *error,
                                            size_t error_size) {
    char dock_error[LOCKDOCKD_ERROR_BUFFER_SIZE] = "";
    char lock_error[LOCKDOCKD_ERROR_BUFFER_SIZE] = "";
    bool dock_restored = true;
    bool lock_restored = true;

    if (snapshot == NULL) {
        lockdockd_set_error(error, error_size, "Internal error");
        return false;
    }

    lockdockd_locker_clear_target();

    if (restore_dock_display) {
        if (!snapshot->has_dock_display) {
            lockdockd_set_error(dock_error, sizeof(dock_error), "Internal error");
            dock_restored = false;
        } else if (!lockdockd_relocate_display(snapshot->dock_display, dock_error,
                                               sizeof(dock_error))) {
            dock_restored = false;
        }
    }

    if (snapshot->locked_display != 0 &&
        !lockdockd_locker_set_target(snapshot->locked_display, lock_error,
                                     sizeof(lock_error))) {
        lock_restored = false;
    }

    if (dock_restored && lock_restored) {
        return true;
    }

    if (!dock_restored && !lock_restored) {
        snprintf(error, error_size, "%s; %s", dock_error, lock_error);
    } else if (!dock_restored) {
        lockdockd_set_error(error, error_size, dock_error);
    } else {
        lockdockd_set_error(error, error_size, lock_error);
    }

    return false;
}

static void lockdockd_set_rollback_error(char *error,
                                         size_t error_size,
                                         const char *operation_error,
                                         const char *preference_rollback_error,
                                         const char *runtime_rollback_error) {
    bool has_preference_rollback_error =
        preference_rollback_error != NULL && preference_rollback_error[0] != '\0';
    bool has_runtime_rollback_error =
        runtime_rollback_error != NULL && runtime_rollback_error[0] != '\0';

    if (!has_preference_rollback_error && !has_runtime_rollback_error) {
        lockdockd_set_error(error, error_size, operation_error);
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

static bool lockdockd_apply_unlock(char *error, size_t error_size) {
    LockDockdStateSnapshot previous_state;
    char operation_error[LOCKDOCKD_ERROR_BUFFER_SIZE];
    char preference_rollback_error[LOCKDOCKD_ERROR_BUFFER_SIZE] = "";

    if (!lockdockd_capture_state_snapshot(&previous_state, NULL, false, error,
                                          error_size)) {
        return false;
    }

    if (!lockdockd_preferences_clear_preferred_display(operation_error,
                                                       sizeof(operation_error))) {
        if (!lockdockd_restore_preferred_display(
                &previous_state, preference_rollback_error,
                sizeof(preference_rollback_error))) {
            lockdockd_set_rollback_error(error, error_size, operation_error,
                                         preference_rollback_error, NULL);
        } else {
            lockdockd_set_error(error, error_size, operation_error);
        }

        return false;
    }

    lockdockd_locker_clear_target();
    return true;
}

static bool lockdockd_apply_set_state(
    const LockDockdStatus *status,
    CGDirectDisplayID display_id,
    const LockDockdDisplayIdentity *preferred_identity,
    char *error,
    size_t error_size) {
    LockDockdStateSnapshot previous_state;
    bool restore_dock_display = false;
    bool clear_existing_lock_for_relocation = false;
    char operation_error[LOCKDOCKD_ERROR_BUFFER_SIZE];
    char preference_rollback_error[LOCKDOCKD_ERROR_BUFFER_SIZE] = "";
    char runtime_rollback_error[LOCKDOCKD_ERROR_BUFFER_SIZE] = "";

    if (preferred_identity == NULL) {
        lockdockd_set_error(error, error_size, "Internal error");
        return false;
    }

    if (!lockdockd_capture_state_snapshot(&previous_state, status, true, error,
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
        lockdockd_locker_clear_target();
    }

    if (restore_dock_display &&
        !lockdockd_relocate_display(display_id, operation_error,
                                    sizeof(operation_error))) {
        if (!lockdockd_restore_runtime_state(&previous_state, restore_dock_display,
                                             runtime_rollback_error,
                                             sizeof(runtime_rollback_error))) {
            lockdockd_set_rollback_error(error, error_size, operation_error, NULL,
                                         runtime_rollback_error);
        } else {
            lockdockd_set_error(error, error_size, operation_error);
        }

        return false;
    }

    if (!lockdockd_locker_set_target(display_id, operation_error,
                                     sizeof(operation_error))) {
        if (!lockdockd_restore_runtime_state(&previous_state, restore_dock_display,
                                             runtime_rollback_error,
                                             sizeof(runtime_rollback_error))) {
            lockdockd_set_rollback_error(error, error_size, operation_error, NULL,
                                         runtime_rollback_error);
        } else {
            lockdockd_set_error(error, error_size, operation_error);
        }

        return false;
    }

    if (!lockdockd_preferences_save_preferred_display(
            preferred_identity, operation_error, sizeof(operation_error))) {
        if (!lockdockd_restore_preferred_display(
                &previous_state, preference_rollback_error,
                sizeof(preference_rollback_error))) {
            /* Keep rolling back runtime even if preferences rollback fails. */
        }

        if (!lockdockd_restore_runtime_state(&previous_state, restore_dock_display,
                                             runtime_rollback_error,
                                             sizeof(runtime_rollback_error))) {
            lockdockd_set_rollback_error(error, error_size, operation_error,
                                         preference_rollback_error,
                                         runtime_rollback_error);
        } else if (preference_rollback_error[0] != '\0') {
            lockdockd_set_rollback_error(error, error_size, operation_error,
                                         preference_rollback_error, NULL);
        } else {
            lockdockd_set_error(error, error_size, operation_error);
        }

        return false;
    }

    return true;
}

static void lockdockd_handle_request(const LockDockdRequest *request,
                                     char *response,
                                     size_t response_size) {
    char error[LOCKDOCKD_ERROR_BUFFER_SIZE];
    LockDockdDisplayIdentity preferred_identity;
    LockDockdStatus status;
    CGDirectDisplayID display_id = 0;

    if (request == NULL) {
        lockdockd_json_error_response(response, response_size, "Internal error");
        return;
    }

    if (request->command == LOCKDOCKD_REQUEST_GET_STATE) {
        if (!lockdockd_build_state_response(response, response_size, error,
                                            sizeof(error))) {
            lockdockd_json_error_response(response, response_size, error);
        }
        return;
    }

    if (request->command == LOCKDOCKD_REQUEST_UNLOCK) {
        if (!lockdockd_apply_unlock(error, sizeof(error))) {
            lockdockd_json_error_response(response, response_size, error);
            return;
        }
        lockdockd_success_response(response, response_size);
        return;
    }

    if (request->command != LOCKDOCKD_REQUEST_SET_STATE) {
        lockdockd_json_error_response(response, response_size, "Unknown command");
        return;
    }

    if (!lockdockd_query_status(&status, error, sizeof(error))) {
        lockdockd_json_error_response(response, response_size, error);
        return;
    }

    if (!lockdockd_resolve_target_display(&status, request->target, &display_id,
                                          error, sizeof(error))) {
        lockdockd_json_error_response(response, response_size, error);
        return;
    }

    if (!lockdockd_copy_display_identity(display_id, &preferred_identity)) {
        lockdockd_json_error_response(response, response_size,
                                      "Could not identify target display");
        return;
    }

    if (!lockdockd_apply_set_state(&status, display_id, &preferred_identity, error,
                                   sizeof(error))) {
        lockdockd_json_error_response(response, response_size, error);
        return;
    }

    lockdockd_success_response(response, response_size);
}

static bool lockdockd_read_request(int fd,
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
    lockdockd_trim_request(buffer);
    if (buffer[0] == '\0') {
        snprintf(error, error_size, "Client sent an empty request");
        return false;
    }

    return true;
}

static bool lockdockd_write_response(int fd,
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

static int lockdockd_probe_existing_socket(const char *socket_path,
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
        snprintf(error, error_size, "lockdockd daemon is already running");
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

static int lockdockd_open_server_socket(char *socket_path,
                                        size_t socket_path_size,
                                        char *error,
                                        size_t error_size) {
    struct sockaddr_un addr;
    int fd = -1;
    int probe_result;

    if (!lockdockd_ipc_ensure_socket_dir(error, error_size) ||
        !lockdockd_ipc_copy_socket_path(socket_path, socket_path_size, error,
                                        error_size)) {
        return -1;
    }

    probe_result = lockdockd_probe_existing_socket(socket_path, error, error_size);
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

    if (listen(fd, 8) != 0) {
        snprintf(error, error_size, "Failed to listen on daemon socket: %s",
                 strerror(errno));
        close(fd);
        unlink(socket_path);
        return -1;
    }

    return fd;
}

static bool lockdockd_register_display_callback(char *error, size_t error_size) {
    CGError cg_error = CGDisplayRegisterReconfigurationCallback(
        lockdockd_display_reconfiguration_callback, NULL);

    if (cg_error == kCGErrorSuccess) {
        return true;
    }

    snprintf(error, error_size,
             "Failed to register display reconfiguration callback (%d)",
             (int)cg_error);
    return false;
}

static void lockdockd_remove_display_callback(void) {
    CGDisplayRemoveReconfigurationCallback(
        lockdockd_display_reconfiguration_callback, NULL);
}

static void lockdockd_close_wakeup_pipe(void) {
    if (g_daemon_wakeup_pipe[0] >= 0) {
        close(g_daemon_wakeup_pipe[0]);
        g_daemon_wakeup_pipe[0] = -1;
    }

    if (g_daemon_wakeup_pipe[1] >= 0) {
        close(g_daemon_wakeup_pipe[1]);
        g_daemon_wakeup_pipe[1] = -1;
    }
}

static bool lockdockd_make_fd_nonblocking(int fd, char *error, size_t error_size) {
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

static bool lockdockd_open_wakeup_pipe(char *error, size_t error_size) {
    if (pipe(g_daemon_wakeup_pipe) != 0) {
        snprintf(error, error_size, "Failed to create daemon wakeup pipe: %s",
                 strerror(errno));
        return false;
    }

    if (!lockdockd_make_fd_nonblocking(g_daemon_wakeup_pipe[0], error, error_size) ||
        !lockdockd_make_fd_nonblocking(g_daemon_wakeup_pipe[1], error, error_size)) {
        lockdockd_close_wakeup_pipe();
        return false;
    }

    return true;
}

static void lockdockd_drain_wakeup_pipe(void) {
    char buffer[64];

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

static void lockdockd_reconcile_pending_display_state(char *error,
                                                      size_t error_size) {
    if (!atomic_exchange(&g_display_state_dirty, false)) {
        return;
    }

    if (!lockdockd_reconcile_display_state(error, error_size)) {
        fprintf(stderr, "Display reconcile failed: %s\n", error);
    }
}

int lockdockd_run_daemon(void) {
    char socket_path[PATH_MAX];
    char error[LOCKDOCKD_ERROR_BUFFER_SIZE];
    int listen_fd = -1;

    signal(SIGINT, lockdockd_signal_handler);
    signal(SIGTERM, lockdockd_signal_handler);
    signal(SIGPIPE, SIG_IGN);

    if (!lockdockd_open_wakeup_pipe(error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }

    listen_fd = lockdockd_open_server_socket(socket_path, sizeof(socket_path), error,
                                             sizeof(error));
    if (listen_fd < 0) {
        fprintf(stderr, "%s\n", error);
        lockdockd_close_wakeup_pipe();
        return 1;
    }

    if (!lockdockd_register_display_callback(error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        close(listen_fd);
        lockdockd_close_wakeup_pipe();
        unlink(socket_path);
        return 1;
    }

    if (!lockdockd_reconcile_display_state(error, sizeof(error))) {
        fprintf(stderr, "Display reconcile failed: %s\n", error);
    }

    printf("lockdockd daemon listening on %s\n", socket_path);

    while (g_daemon_running) {
        fd_set read_fds;
        int select_result;
        int max_fd = listen_fd;

        if (g_daemon_wakeup_pipe[0] > max_fd) {
            max_fd = g_daemon_wakeup_pipe[0];
        }

        FD_ZERO(&read_fds);
        FD_SET(listen_fd, &read_fds);
        FD_SET(g_daemon_wakeup_pipe[0], &read_fds);

        select_result = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (select_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            fprintf(stderr, "Daemon select failed: %s\n", strerror(errno));
            break;
        }

        if (FD_ISSET(g_daemon_wakeup_pipe[0], &read_fds)) {
            lockdockd_drain_wakeup_pipe();
        }

        if (!g_daemon_running) {
            break;
        }

        lockdockd_reconcile_pending_display_state(error, sizeof(error));

        if (select_result > 0 && FD_ISSET(listen_fd, &read_fds)) {
            int client_fd = accept(listen_fd, NULL, NULL);

            if (client_fd >= 0) {
                char request[LOCKDOCKD_IPC_MAX_MESSAGE];
                char response[LOCKDOCKD_IPC_MAX_MESSAGE];

                if (!lockdockd_read_request(client_fd, request, sizeof(request),
                                            error, sizeof(error))) {
                    lockdockd_json_error_response(response, sizeof(response), error);
                } else {
                    LockDockdRequest parsed_request;

                    if (!lockdockd_parse_request_json(request, &parsed_request,
                                                      error, sizeof(error))) {
                        lockdockd_json_error_response(response, sizeof(response),
                                                      error);
                    } else {
                        lockdockd_handle_request(&parsed_request, response,
                                                 sizeof(response));
                    }
                }

                if (!lockdockd_write_response(client_fd, response, error,
                                              sizeof(error))) {
                    fprintf(stderr, "%s\n", error);
                }

                close(client_fd);
            }
        }

        lockdockd_reconcile_pending_display_state(error, sizeof(error));
    }

    lockdockd_remove_display_callback();
    lockdockd_locker_shutdown();
    lockdockd_close_wakeup_pipe();
    if (listen_fd >= 0) {
        close(listen_fd);
    }
    unlink(socket_path);
    return 0;
}
