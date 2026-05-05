#define _DARWIN_C_SOURCE 1
#define _XOPEN_SOURCE 700

#include "test_support.h"

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool lockdock_test_push_env(LockDockTestEnvGuard *guard,
                            const char *name,
                            const char *value) {
    const char *previous_value;

    if (guard == NULL || name == NULL || name[0] == '\0' || value == NULL) {
        return false;
    }

    memset(guard, 0, sizeof(*guard));
    if (snprintf(guard->name, sizeof(guard->name), "%s", name) >=
        (int)sizeof(guard->name)) {
        return false;
    }

    previous_value = getenv(name);
    if (previous_value != NULL) {
        guard->had_previous_value = true;
        if (snprintf(guard->previous_value, sizeof(guard->previous_value), "%s",
                     previous_value) >= (int)sizeof(guard->previous_value)) {
            return false;
        }
    }

    return setenv(name, value, 1) == 0;
}

void lockdock_test_pop_env(const LockDockTestEnvGuard *guard) {
    if (guard == NULL || guard->name[0] == '\0') {
        return;
    }

    if (guard->had_previous_value) {
        setenv(guard->name, guard->previous_value, 1);
    } else {
        unsetenv(guard->name);
    }
}

bool lockdock_test_make_temp_dir(char *buffer, size_t buffer_size) {
    char template[] = "./t.XXXXXX";
    char resolved_path[PATH_MAX];
    char *path;

    if (buffer == NULL || buffer_size == 0) {
        return false;
    }

    path = mkdtemp(template);
    if (path == NULL) {
        return false;
    }

    if (realpath(path, resolved_path) == NULL) {
        rmdir(path);
        return false;
    }

    if (snprintf(buffer, buffer_size, "%s", resolved_path) >= (int)buffer_size) {
        rmdir(path);
        return false;
    }

    return true;
}

bool lockdock_test_join_path(char *buffer,
                             size_t buffer_size,
                             const char *left,
                             const char *right) {
    if (buffer == NULL || buffer_size == 0 || left == NULL || right == NULL) {
        return false;
    }

    return snprintf(buffer, buffer_size, "%s/%s", left, right) < (int)buffer_size;
}

bool lockdock_test_read_file(const char *path, char *buffer, size_t buffer_size) {
    FILE *file;
    size_t used = 0;

    if (path == NULL || buffer == NULL || buffer_size < 2) {
        return false;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }

    while (used + 1 < buffer_size) {
        size_t nread = fread(buffer + used, 1, buffer_size - used - 1, file);

        used += nread;
        if (nread == 0) {
            if (ferror(file)) {
                fclose(file);
                return false;
            }
            break;
        }
    }

    if (!feof(file)) {
        fclose(file);
        return false;
    }

    buffer[used] = '\0';
    fclose(file);
    return true;
}

static int lockdock_test_remove_tree_entry(const char *path,
                                           const struct stat *info,
                                           int typeflag,
                                           struct FTW *state) {
    (void)info;
    (void)typeflag;
    (void)state;

    return remove(path);
}

bool lockdock_test_remove_tree(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return false;
    }

    if (access(path, F_OK) != 0) {
        return errno == ENOENT;
    }

    return nftw(path, lockdock_test_remove_tree_entry, 16, FTW_DEPTH | FTW_PHYS) ==
           0;
}
