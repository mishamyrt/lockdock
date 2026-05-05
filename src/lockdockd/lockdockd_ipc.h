#ifndef LOCKDOCKD_IPC_H
#define LOCKDOCKD_IPC_H

#include <stdbool.h>
#include <stddef.h>

#define LOCKDOCKD_IPC_BUNDLE_ID "co.myrt.lockdockd"
#define LOCKDOCKD_IPC_MAX_MESSAGE 4096

bool lockdockd_ipc_ensure_socket_dir(char *error, size_t error_size);
bool lockdockd_ipc_copy_socket_path(char *buffer,
                                    size_t buffer_size,
                                    char *error,
                                    size_t error_size);

#endif
