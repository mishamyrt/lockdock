#include "lockdockd_ipc.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static void lockdockd_set_error(char *buffer,
                                size_t buffer_size,
                                const char *message) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", message);
}

static const char *lockdockd_home_dir(void) {
    const char *home = getenv("HOME");

    if (home == NULL || home[0] == '\0') {
        return NULL;
    }

    return home;
}

static bool lockdockd_copy_socket_dir(char *buffer,
                                      size_t buffer_size,
                                      char *error,
                                      size_t error_size) {
    const char *home = lockdockd_home_dir();

    if (home == NULL) {
        lockdockd_set_error(error, error_size, "HOME is not set");
        return false;
    }

    if (snprintf(buffer, buffer_size, "%s/Library/Caches/%s", home,
                 LOCKDOCKD_IPC_BUNDLE_ID) >= (int)buffer_size) {
        lockdockd_set_error(error, error_size, "Socket directory path is too long");
        return false;
    }

    return true;
}

static bool lockdockd_mkdir_p(const char *path, char *error, size_t error_size) {
    char tmp[PATH_MAX];
    size_t length;

    if (path == NULL || path[0] == '\0') {
        lockdockd_set_error(error, error_size, "Socket directory path is empty");
        return false;
    }

    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
        lockdockd_set_error(error, error_size, "Socket directory path is too long");
        return false;
    }

    length = strlen(tmp);
    for (size_t i = 1; i < length; i++) {
        if (tmp[i] != '/') {
            continue;
        }

        tmp[i] = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
            snprintf(error, error_size, "Failed to create socket directory '%s': %s",
                     tmp, strerror(errno));
            return false;
        }
        tmp[i] = '/';
    }

    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
        snprintf(error, error_size, "Failed to create socket directory '%s': %s",
                 tmp, strerror(errno));
        return false;
    }

    return true;
}

bool lockdockd_ipc_ensure_socket_dir(char *error, size_t error_size) {
    char dir_path[PATH_MAX];

    if (!lockdockd_copy_socket_dir(dir_path, sizeof(dir_path), error, error_size)) {
        return false;
    }

    return lockdockd_mkdir_p(dir_path, error, error_size);
}

bool lockdockd_ipc_copy_socket_path(char *buffer,
                                    size_t buffer_size,
                                    char *error,
                                    size_t error_size) {
    char dir_path[PATH_MAX];

    if (!lockdockd_copy_socket_dir(dir_path, sizeof(dir_path), error, error_size)) {
        return false;
    }

    if (snprintf(buffer, buffer_size, "%s/control.sock", dir_path) >=
        (int)buffer_size) {
        lockdockd_set_error(error, error_size, "Socket path is too long");
        return false;
    }

    return true;
}

static bool lockdockd_socket_disable_sigpipe(int fd) {
    int one = 1;

    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) == 0;
}

static bool lockdockd_write_all(int fd,
                                const char *buffer,
                                size_t length,
                                char *error,
                                size_t error_size) {
    size_t offset = 0;

    while (offset < length) {
        ssize_t written = write(fd, buffer + offset, length - offset);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }

            snprintf(error, error_size, "Socket write failed: %s", strerror(errno));
            return false;
        }

        offset += (size_t)written;
    }

    return true;
}

static bool lockdockd_read_response(int fd,
                                    char *response,
                                    size_t response_size,
                                    char *error,
                                    size_t error_size) {
    size_t used = 0;

    while (used + 1 < response_size) {
        ssize_t nread = read(fd, response + used, response_size - used - 1);

        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }

            snprintf(error, error_size, "Socket read failed: %s", strerror(errno));
            return false;
        }

        if (nread == 0) {
            break;
        }

        used += (size_t)nread;
    }

    if (used == 0) {
        lockdockd_set_error(error, error_size, "Daemon returned an empty response");
        return false;
    }

    if (used + 1 >= response_size) {
        lockdockd_set_error(error, error_size, "Daemon response exceeded buffer");
        return false;
    }

    while (used > 0 && (response[used - 1] == '\n' || response[used - 1] == '\r' ||
                        response[used - 1] == ' ' || response[used - 1] == '\t')) {
        used--;
    }

    response[used] = '\0';
    return true;
}

int lockdockd_ipc_send_request(const char *request,
                               char *response,
                               size_t response_size,
                               char *error,
                               size_t error_size) {
    char socket_path[PATH_MAX];
    char request_buffer[LOCKDOCKD_IPC_MAX_MESSAGE];
    struct sockaddr_un addr;
    int fd = -1;
    int exit_code = -1;

    if (request == NULL || response == NULL || response_size == 0) {
        lockdockd_set_error(error, error_size, "Internal error");
        return -1;
    }

    if (!lockdockd_ipc_copy_socket_path(socket_path, sizeof(socket_path), error,
                                        error_size)) {
        return -1;
    }

    if (snprintf(request_buffer, sizeof(request_buffer), "%s\n", request) >=
        (int)sizeof(request_buffer)) {
        lockdockd_set_error(error, error_size, "IPC request is too long");
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(error, error_size, "Failed to create client socket: %s",
                 strerror(errno));
        return -1;
    }

    signal(SIGPIPE, SIG_IGN);
    lockdockd_socket_disable_sigpipe(fd);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_len = sizeof(addr);
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (errno == ENOENT || errno == ECONNREFUSED) {
            snprintf(error, error_size,
                     "lockdockd daemon is not running. Start it with '%s'",
                     "lockdockd");
        } else {
            snprintf(error, error_size, "Failed to connect to daemon: %s",
                     strerror(errno));
        }
        goto cleanup;
    }

    if (!lockdockd_write_all(fd, request_buffer, strlen(request_buffer), error,
                             error_size)) {
        goto cleanup;
    }

    shutdown(fd, SHUT_WR);
    if (!lockdockd_read_response(fd, response, response_size, error, error_size)) {
        goto cleanup;
    }

    exit_code = 0;

cleanup:
    if (fd >= 0) {
        close(fd);
    }

    return exit_code;
}

bool lockdockd_ipc_response_is_success(const char *response) {
    return response != NULL && strcmp(response, "{\"success\":true}") == 0;
}

bool lockdockd_ipc_response_is_error(const char *response) {
    return response != NULL && strstr(response, "\"success\":false") != NULL;
}

static const char *lockdockd_find_json_key(const char *json, const char *key) {
    static char pattern[64];

    if (json == NULL || key == NULL) {
        return NULL;
    }

    if (snprintf(pattern, sizeof(pattern), "\"%s\":", key) >= (int)sizeof(pattern)) {
        return NULL;
    }

    return strstr(json, pattern);
}

static bool lockdockd_json_decode_string(const char *start,
                                         char *buffer,
                                         size_t buffer_size) {
    size_t used = 0;
    const char *cursor = start;

    if (buffer == NULL || buffer_size == 0 || cursor == NULL || *cursor != '"') {
        return false;
    }

    cursor++;
    while (*cursor != '\0' && *cursor != '"') {
        char ch = *cursor++;

        if (ch == '\\') {
            char escaped = *cursor++;

            if (escaped == '\0') {
                return false;
            }

            if (escaped == '"' || escaped == '\\' || escaped == '/') {
                ch = escaped;
            } else if (escaped == 'n') {
                ch = '\n';
            } else if (escaped == 'r') {
                ch = '\r';
            } else if (escaped == 't') {
                ch = '\t';
            } else {
                return false;
            }
        }

        if (used + 1 >= buffer_size) {
            return false;
        }

        buffer[used++] = ch;
    }

    if (*cursor != '"') {
        return false;
    }

    buffer[used] = '\0';
    return true;
}

bool lockdockd_ipc_extract_error(const char *response,
                                 char *error,
                                 size_t error_size) {
    const char *key = lockdockd_find_json_key(response, "error");
    const char *value;

    if (key == NULL) {
        lockdockd_set_error(error, error_size,
                            "Daemon returned an unexpected response");
        return false;
    }

    value = key + strlen("\"error\":");
    while (*value == ' ' || *value == '\t') {
        value++;
    }

    if (!lockdockd_json_decode_string(value, error, error_size)) {
        lockdockd_set_error(error, error_size, "Failed to decode daemon error");
        return false;
    }

    return true;
}

static bool lockdockd_parse_json_int(const char *json,
                                     const char *key,
                                     int *value_out) {
    const char *cursor = lockdockd_find_json_key(json, key);
    char *endptr = NULL;
    long value;

    if (cursor == NULL || value_out == NULL) {
        return false;
    }

    cursor += strlen(key) + 3;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }

    value = strtol(cursor, &endptr, 10);
    if (endptr == cursor) {
        return false;
    }

    *value_out = (int)value;
    return true;
}

bool lockdockd_ipc_parse_status_indices(const char *response,
                                        int *location_out,
                                        bool *has_target_out,
                                        int *target_out,
                                        char *error,
                                        size_t error_size) {
    int target = -1;

    if (response == NULL || location_out == NULL || has_target_out == NULL ||
        target_out == NULL) {
        lockdockd_set_error(error, error_size, "Internal error");
        return false;
    }

    if (lockdockd_ipc_response_is_error(response)) {
        lockdockd_ipc_extract_error(response, error, error_size);
        return false;
    }

    if (!lockdockd_parse_json_int(response, "location", location_out)) {
        lockdockd_set_error(error, error_size, "Failed to parse daemon status");
        return false;
    }

    if (lockdockd_parse_json_int(response, "target", &target)) {
        *has_target_out = true;
        *target_out = target;
    } else {
        *has_target_out = false;
        *target_out = -1;
    }

    return true;
}
