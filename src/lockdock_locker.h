#ifndef LOCKDOCK_LOCKER_H
#define LOCKDOCK_LOCKER_H

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stddef.h>

bool lockdock_locker_set_target(CGDirectDisplayID display_id,
                                char *error,
                                size_t error_size);
void lockdock_locker_refresh_display_cache(void);
void lockdock_locker_clear_target(void);
CGDirectDisplayID lockdock_locker_get_target(void);
void lockdock_locker_shutdown(void);

#endif
