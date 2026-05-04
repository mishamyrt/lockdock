#ifndef LOCKDOCKD_OBJC_BRIDGE_H
#define LOCKDOCKD_OBJC_BRIDGE_H

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stddef.h>

#include "lockdockd_display.h"

bool lockdockd_copy_display_name(CGDirectDisplayID display_id,
                                 char *buffer,
                                 size_t buffer_size);
LockDockdDockOrientation lockdockd_get_dock_orientation(void);
CGDirectDisplayID lockdockd_get_dock_display(void);

#endif
