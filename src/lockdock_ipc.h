#ifndef LOCKDOCK_IPC_H
#define LOCKDOCK_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOCKDOCK_IPC_MAX_MESSAGE 4096
#define LOCKDOCK_IPC_MAX_ERROR 1024
#define LOCKDOCK_IPC_MAX_DISPLAYS 32
#define LOCKDOCK_IPC_DISPLAY_NAME_SIZE 256
#define LOCKDOCK_IPC_REASON_SIZE 512

typedef enum : uint8_t {
    LOCKDOCK_IPC_COMMAND_NONE = 0,
    LOCKDOCK_IPC_COMMAND_GET_STATE,
    LOCKDOCK_IPC_COMMAND_SET_STATE,
    LOCKDOCK_IPC_COMMAND_UNLOCK
} LockDockIpcCommand;

typedef struct {
    LockDockIpcCommand command;
    bool has_target;
    int target;
} LockDockIpcRequest;

typedef struct {
    char displays[LOCKDOCK_IPC_MAX_DISPLAYS][LOCKDOCK_IPC_DISPLAY_NAME_SIZE];
    int location_index;
    int target_index;
    uint8_t display_count;
    bool has_target;
} LockDockIpcState;

typedef struct {
    bool success;
    char reason[LOCKDOCK_IPC_REASON_SIZE];
} LockDockIpcResult;

enum : uint8_t {
    LOCKDOCK_IPC_RESPONSE_NONE = 0,
    LOCKDOCK_IPC_RESPONSE_STATE,
    LOCKDOCK_IPC_RESPONSE_RESULT
};

typedef uint8_t LockDockIpcResponseKind;

typedef struct {
    LockDockIpcResponseKind kind;
    union {
        LockDockIpcState state;
        LockDockIpcResult result;
    };
} LockDockIpcResponse;

bool lockdock_ipc_ensure_socket_dir(char *error, size_t error_size);
bool lockdock_ipc_copy_socket_path(char *buffer,
                                   size_t buffer_size,
                                   char *error,
                                   size_t error_size);

bool lockdock_ipc_parse_request_json(const char *request_json,
                                     LockDockIpcRequest *request_out,
                                     char *error,
                                     size_t error_size);
bool lockdock_ipc_serialize_request_json(const LockDockIpcRequest *request,
                                         char *buffer,
                                         size_t buffer_size,
                                         char *error,
                                         size_t error_size);

bool lockdock_ipc_parse_response_json(const char *response_json,
                                      LockDockIpcResponse *response_out,
                                      char *error,
                                      size_t error_size);
bool lockdock_ipc_serialize_state_response_json(const LockDockIpcState *state,
                                                char *buffer,
                                                size_t buffer_size,
                                                char *error,
                                                size_t error_size);
bool lockdock_ipc_serialize_result_response_json(const LockDockIpcResult *result,
                                                 char *buffer,
                                                 size_t buffer_size,
                                                 char *error,
                                                 size_t error_size);

bool lockdock_ipc_get_state(LockDockIpcState *state_out,
                            char *error,
                            size_t error_size);
bool lockdock_ipc_lock(int target_index, char *error, size_t error_size);
bool lockdock_ipc_unlock(char *error, size_t error_size);

#endif
