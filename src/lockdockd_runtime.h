#ifndef LOCKDOCKD_RUNTIME_H
#define LOCKDOCKD_RUNTIME_H

#include "lockdockd_display.h"

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stddef.h>

#define LOCKDOCKD_DISPLAY_NAME_BUFFER_SIZE 256
#define LOCKDOCKD_ERROR_BUFFER_SIZE 512

typedef struct {
    CGDirectDisplayID displays[LOCKDOCKD_MAX_DISPLAYS];
    uint32_t display_count;
    int location_index;
} LockDockdStatus;

bool lockdockd_copy_display_label(CGDirectDisplayID display_id,
                                  char *buffer,
                                  size_t buffer_size);
bool lockdockd_query_status(LockDockdStatus *status, char *error, size_t error_size);
int lockdockd_status_index_for_display(const LockDockdStatus *status,
                                       CGDirectDisplayID display_id);
bool lockdockd_relocate_display(CGDirectDisplayID display_id,
                                char *error,
                                size_t error_size);

#endif
