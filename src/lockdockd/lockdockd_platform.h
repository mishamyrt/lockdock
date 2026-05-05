#ifndef LOCKDOCKD_OBJC_BRIDGE_H
#define LOCKDOCKD_OBJC_BRIDGE_H

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stddef.h>

#include "lockdockd_display.h"

typedef struct {
    bool has_window_bounds;
    CGRect window_bounds;
    CGDirectDisplayID window_display;
} LockDockdDockProbe;

bool lockdockd_copy_display_name(CGDirectDisplayID display_id,
                                 char *buffer,
                                 size_t buffer_size);
void lockdockd_invalidate_display_name_cache(void);
void lockdockd_invalidate_dock_orientation_cache(void);
bool lockdockd_is_accessibility_trusted(void);
void lockdockd_reset_dock_probe(LockDockdDockProbe *probe);
bool lockdockd_capture_dock_probe(LockDockdDockProbe *probe);
CGDirectDisplayID lockdockd_resolve_dock_probe(const LockDockdDockProbe *probe,
                                               bool allow_slow_fallback);
LockDockdDockOrientation lockdockd_get_dock_orientation(void);
CGDirectDisplayID lockdockd_get_dock_display(void);

#endif
