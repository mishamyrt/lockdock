#ifndef LOCKDOCKD_LOCKER_H
#define LOCKDOCKD_LOCKER_H

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stddef.h>

bool lockdockd_locker_set_target(CGDirectDisplayID display_id,
                                 char *error,
                                 size_t error_size);
void lockdockd_locker_clear_target(void);
CGDirectDisplayID lockdockd_locker_get_target(void);
void lockdockd_locker_shutdown(void);

#endif
