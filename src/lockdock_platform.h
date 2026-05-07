#ifndef lockdock_OBJC_BRIDGE_H
#define lockdock_OBJC_BRIDGE_H

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stddef.h>

#include "lockdock_display.h"

typedef struct {
    bool has_window_bounds;
    CGRect window_bounds;
    CGDirectDisplayID window_display;
} LockDockDockProbe;

bool lockdock_copy_display_name(CGDirectDisplayID display_id,
                                char *buffer,
                                size_t buffer_size);
void lockdock_invalidate_display_name_cache(void);
void lockdock_invalidate_dock_orientation_cache(void);
bool lockdock_is_accessibility_trusted(void);
void lockdock_reset_dock_probe(LockDockDockProbe *probe);
bool lockdock_capture_dock_probe(LockDockDockProbe *probe);
CGDirectDisplayID lockdock_resolve_dock_probe(const LockDockDockProbe *probe,
                                              bool allow_slow_fallback);
LockDockDockOrientation lockdock_get_dock_orientation(void);
CGDirectDisplayID lockdock_get_dock_display(void);

#endif
