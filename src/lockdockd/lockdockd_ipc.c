#include "lockdockd_ipc.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
