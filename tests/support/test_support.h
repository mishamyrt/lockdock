#ifndef LOCKDOCK_TEST_SUPPORT_H
#define LOCKDOCK_TEST_SUPPORT_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char name[64];
    char previous_value[PATH_MAX];
    bool had_previous_value;
} LockDockTestEnvGuard;

bool lockdock_test_push_env(LockDockTestEnvGuard *guard,
                            const char *name,
                            const char *value);
void lockdock_test_pop_env(const LockDockTestEnvGuard *guard);

bool lockdock_test_make_temp_dir(char *buffer, size_t buffer_size);
bool lockdock_test_join_path(char *buffer,
                             size_t buffer_size,
                             const char *left,
                             const char *right);
bool lockdock_test_read_file(const char *path, char *buffer, size_t buffer_size);
bool lockdock_test_remove_tree(const char *path);

#endif
