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
int lockdockd_ipc_send_request(const char *request,
                               char *response,
                               size_t response_size,
                               char *error,
                               size_t error_size);
bool lockdockd_ipc_response_is_success(const char *response);
bool lockdockd_ipc_response_is_error(const char *response);
bool lockdockd_ipc_extract_error(const char *response,
                                 char *error,
                                 size_t error_size);
bool lockdockd_ipc_parse_status_indices(const char *response,
                                        int *location_out,
                                        bool *has_target_out,
                                        int *target_out,
                                        char *error,
                                        size_t error_size);

#endif
