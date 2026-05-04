#include "lockdockd_daemon.h"

#include "lockdockd_ipc.h"
#include "lockdockd_locker.h"
#include "lockdockd_runtime.h"

#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static volatile sig_atomic_t g_daemon_running = 1;

static void lockdockd_signal_handler(int signal_number) {
    if (signal_number == SIGINT || signal_number == SIGTERM) {
        g_daemon_running = 0;
    }
}

static void lockdockd_append_error(char *buffer,
                                   size_t buffer_size,
                                   const char *message) {
    snprintf(buffer, buffer_size, "{\"success\":false,\"error\":\"%s\"}", message);
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
                                "{\"success\":false,\"error\":",
                                strlen("{\"success\":false,\"error\":")) ||
        !lockdockd_append_json_string(buffer, buffer_size, &used, message) ||
        !lockdockd_append_bytes(buffer, buffer_size, &used, "}", 1)) {
        lockdockd_append_error(buffer, buffer_size, "Internal error");
    }
}

static bool lockdockd_build_status_response(char *buffer,
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
        snprintf(error, error_size, "Status response buffer is too small");
        return false;
    }

    for (uint32_t i = 0; i < status.display_count; i++) {
        char display_name[LOCKDOCKD_DISPLAY_NAME_BUFFER_SIZE];

        lockdockd_copy_display_label(status.displays[i], display_name,
                                     sizeof(display_name));

        if (i > 0 && !lockdockd_append_bytes(buffer, buffer_size, &used, ",", 1)) {
            snprintf(error, error_size, "Status response buffer is too small");
            return false;
        }

        if (!lockdockd_append_json_string(buffer, buffer_size, &used,
                                          display_name)) {
            snprintf(error, error_size, "Status response buffer is too small");
            return false;
        }
    }

    if (!lockdockd_append_format(buffer, buffer_size, &used, "],\"location\":%d",
                                 status.location_index)) {
        snprintf(error, error_size, "Status response buffer is too small");
        return false;
    }

    if (target_index >= 0 &&
        !lockdockd_append_format(buffer, buffer_size, &used, ",\"target\":%d",
                                 target_index)) {
        snprintf(error, error_size, "Status response buffer is too small");
        return false;
    }

    if (!lockdockd_append_bytes(buffer, buffer_size, &used, "}", 1)) {
        snprintf(error, error_size, "Status response buffer is too small");
        return false;
    }

    return true;
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

static bool lockdockd_parse_request(char *request,
                                    char **command_out,
                                    char **arg_out) {
    char *cursor = request;
    char *space;

    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }

    if (*cursor == '\0') {
        return false;
    }

    space = strpbrk(cursor, " \t");
    if (space == NULL) {
        *command_out = cursor;
        *arg_out = NULL;
        return true;
    }

    *space = '\0';
    space++;
    while (*space == ' ' || *space == '\t') {
        space++;
    }

    *command_out = cursor;
    *arg_out = *space == '\0' ? NULL : space;
    return true;
}

static bool lockdockd_request_has_extra_args(char *arg) {
    return arg != NULL && strpbrk(arg, " \t") != NULL;
}

static void lockdockd_handle_request(const char *command,
                                     const char *arg,
                                     char *response,
                                     size_t response_size) {
    char error[LOCKDOCKD_ERROR_BUFFER_SIZE];
    CGDirectDisplayID display_id = 0;
    CGDirectDisplayID target_display = lockdockd_locker_get_target();

    if (strcmp(command, "status") == 0) {
        if (arg != NULL) {
            lockdockd_json_error_response(response, response_size,
                                          "status does not take arguments");
            return;
        }

        if (!lockdockd_build_status_response(response, response_size, error,
                                             sizeof(error))) {
            lockdockd_json_error_response(response, response_size, error);
        }
        return;
    }

    if (strcmp(command, "unlock") == 0) {
        if (arg != NULL) {
            lockdockd_json_error_response(response, response_size,
                                          "unlock does not take arguments");
            return;
        }

        lockdockd_locker_clear_target();
        lockdockd_success_response(response, response_size);
        return;
    }

    if (arg == NULL || lockdockd_request_has_extra_args((char *)arg)) {
        lockdockd_json_error_response(
            response, response_size,
            "Command requires exactly one display argument");
        return;
    }

    if (!lockdockd_resolve_display_token(arg, &display_id, error, sizeof(error))) {
        lockdockd_json_error_response(response, response_size, error);
        return;
    }

    if (strcmp(command, "lock") == 0) {
        if (!lockdockd_locker_set_target(display_id, error, sizeof(error))) {
            lockdockd_json_error_response(response, response_size, error);
            return;
        }

        lockdockd_success_response(response, response_size);
        return;
    }

    if (strcmp(command, "relocate") == 0) {
        if (target_display != 0 && target_display != display_id &&
            !lockdockd_locker_set_target(display_id, error, sizeof(error))) {
            lockdockd_json_error_response(response, response_size, error);
            return;
        }

        if (!lockdockd_relocate_display(display_id, error, sizeof(error))) {
            lockdockd_json_error_response(response, response_size, error);
            return;
        }

        lockdockd_success_response(response, response_size);
        return;
    }

    lockdockd_json_error_response(response, response_size, "Unknown command");
}

static bool lockdockd_read_request(int fd,
                                   char *buffer,
                                   size_t buffer_size,
                                   char *error,
                                   size_t error_size) {
    size_t used = 0;

    while (used + 1 < buffer_size) {
        ssize_t nread = read(fd, buffer + used, 1);

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

        if (buffer[used] == '\n') {
            used++;
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

    if (write(fd, "\n", 1) < 0 && errno != EPIPE) {
        snprintf(error, error_size, "Failed to finalize client response: %s",
                 strerror(errno));
        return false;
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

static void lockdockd_pump_run_loop(void) {
    while (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true) ==
           kCFRunLoopRunHandledSource) {
    }
}

int lockdockd_run_daemon(void) {
    char socket_path[PATH_MAX];
    char error[LOCKDOCKD_ERROR_BUFFER_SIZE];
    int listen_fd = -1;

    signal(SIGINT, lockdockd_signal_handler);
    signal(SIGTERM, lockdockd_signal_handler);
    signal(SIGPIPE, SIG_IGN);

    listen_fd = lockdockd_open_server_socket(socket_path, sizeof(socket_path), error,
                                             sizeof(error));
    if (listen_fd < 0) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }

    printf("lockdockd daemon listening on %s\n", socket_path);

    while (g_daemon_running) {
        fd_set read_fds;
        struct timeval timeout;
        int select_result;

        FD_ZERO(&read_fds);
        FD_SET(listen_fd, &read_fds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 50000;

        select_result = select(listen_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (select_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            fprintf(stderr, "Daemon select failed: %s\n", strerror(errno));
            break;
        }

        if (select_result > 0 && FD_ISSET(listen_fd, &read_fds)) {
            int client_fd = accept(listen_fd, NULL, NULL);

            if (client_fd >= 0) {
                char request[LOCKDOCKD_IPC_MAX_MESSAGE];
                char response[LOCKDOCKD_IPC_MAX_MESSAGE];

                if (!lockdockd_read_request(client_fd, request, sizeof(request),
                                            error, sizeof(error))) {
                    lockdockd_json_error_response(response, sizeof(response), error);
                } else {
                    char *command = NULL;
                    char *arg = NULL;

                    if (!lockdockd_parse_request(request, &command, &arg)) {
                        lockdockd_json_error_response(response, sizeof(response),
                                                      "Invalid request");
                    } else {
                        lockdockd_handle_request(command, arg, response,
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

        lockdockd_pump_run_loop();
    }

    lockdockd_locker_shutdown();
    if (listen_fd >= 0) {
        close(listen_fd);
    }
    unlink(socket_path);
    return 0;
}
