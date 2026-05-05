#ifndef LOCKDOCKD_REQUEST_H
#define LOCKDOCKD_REQUEST_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LOCKDOCKD_REQUEST_NONE = 0,
    LOCKDOCKD_REQUEST_GET_STATE,
    LOCKDOCKD_REQUEST_SET_STATE,
    LOCKDOCKD_REQUEST_UNLOCK
} LockDockdRequestCommand;

typedef struct {
    LockDockdRequestCommand command;
    bool has_target;
    int target;
} LockDockdRequest;

bool lockdockd_parse_request_json(const char *request_json,
                                  LockDockdRequest *request_out,
                                  char *error,
                                  size_t error_size);

#endif
