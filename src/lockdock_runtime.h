#ifndef lockdock_RUNTIME_H
#define lockdock_RUNTIME_H

#include "lockdock_display.h"

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stddef.h>

#define lockdock_DISPLAY_NAME_BUFFER_SIZE 256
#define lockdock_ERROR_BUFFER_SIZE 512

typedef struct {
    CGDirectDisplayID displays[lockdock_MAX_DISPLAYS];
    uint32_t display_count;
    int location_index;
} LockDockStatus;

bool lockdock_copy_display_label(CGDirectDisplayID display_id,
                                 char *buffer,
                                 size_t buffer_size);
bool lockdock_query_status(LockDockStatus *status, char *error, size_t error_size);
int lockdock_status_index_for_display(const LockDockStatus *status,
                                      CGDirectDisplayID display_id);
bool lockdock_relocate_display(CGDirectDisplayID display_id,
                               char *error,
                               size_t error_size);

#endif
