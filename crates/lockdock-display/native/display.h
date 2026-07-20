#ifndef LOCKDOCK_DISPLAY_H
#define LOCKDOCK_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    double x;
    double y;
    double width;
    double height;
} LockDockDisplayRect;

uint32_t lockdock_display_get_active_displays(uint32_t *displays,
                                              uint32_t max_displays);
bool lockdock_display_copy_bounds(uint32_t display_id,
                                  LockDockDisplayRect *rect_out);
bool lockdock_display_is_accessibility_trusted(void);
bool lockdock_display_copy_dock_window_bounds(LockDockDisplayRect *rect_out);
bool lockdock_display_copy_accessibility_dock_window_bounds(
    LockDockDisplayRect *rect_out);
bool lockdock_display_dock_overlay_active(uint32_t display_id);

#endif
