#ifndef LOCKDOCK_DISPLAY_SHIM_H
#define LOCKDOCK_DISPLAY_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    double x;
    double y;
    double width;
    double height;
} LockDockShimRect;

typedef struct {
    double x;
    double y;
} LockDockShimPoint;

typedef enum {
    LOCKDOCK_SHIM_EVENT_OTHER = 0,
    LOCKDOCK_SHIM_EVENT_MOUSE_MOVED = 1,
    LOCKDOCK_SHIM_EVENT_MOUSE_DRAGGED = 2,
} LockDockShimEventKind;

uint32_t lockdock_shim_get_active_displays(uint32_t *displays,
                                           uint32_t max_displays);
bool lockdock_shim_copy_display_bounds(uint32_t display_id,
                                       LockDockShimRect *rect_out);
bool lockdock_shim_copy_display_uuid(uint32_t display_id,
                                     char *buffer,
                                     size_t buffer_size);
bool lockdock_shim_display_is_builtin(uint32_t display_id);
uint32_t lockdock_shim_display_vendor_number(uint32_t display_id);
uint32_t lockdock_shim_display_model_number(uint32_t display_id);
uint32_t lockdock_shim_display_serial_number(uint32_t display_id);
bool lockdock_shim_is_accessibility_trusted(void);
bool lockdock_shim_copy_dock_window_bounds(LockDockShimRect *rect_out);
bool lockdock_shim_copy_accessibility_dock_window_bounds(LockDockShimRect *rect_out);
bool lockdock_shim_copy_dock_orientation(char *buffer, size_t buffer_size);
bool lockdock_shim_copy_mouse_location(LockDockShimPoint *point_out);
void *lockdock_shim_event_source_create(void);
void lockdock_shim_event_source_set_suppression_interval(void *source,
                                                         double interval);
void lockdock_shim_release(void *object);
bool lockdock_shim_set_cursor_association(bool associated);
void lockdock_shim_warp_mouse(LockDockShimPoint point);
void lockdock_shim_post_mouse_moved(void *source, LockDockShimPoint point);
void lockdock_shim_post_edge_nudge(void *source,
                                   LockDockShimPoint point,
                                   int orientation);
bool lockdock_shim_start_event_tap(char *error, size_t error_size);
void lockdock_shim_stop_event_tap(void);

extern bool lockdock_display_should_suppress_event(int event_kind,
                                                   double x,
                                                   double y);

#endif
