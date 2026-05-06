#include "lockdockd_runtime.h"

#include "lockdockd_platform.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void lockdockd_set_error(char *buffer,
                                size_t buffer_size,
                                const char *message) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", message);
}

bool lockdockd_copy_display_label(CGDirectDisplayID display_id,
                                  char *buffer,
                                  size_t buffer_size) {
    if (buffer == NULL || buffer_size == 0) {
        return false;
    }

    if (CGDisplayIsBuiltin(display_id)) {
        snprintf(buffer, buffer_size, "Built-in Display");
        return true;
    }

    if (lockdockd_copy_display_name(display_id, buffer, buffer_size)) {
        return true;
    }

    snprintf(buffer, buffer_size, "Display-%u", display_id);
    return true;
}

static bool lockdockd_validate_display(CGDirectDisplayID display_id,
                                       char *error,
                                       size_t error_size) {
    CGRect bounds = CGDisplayBounds(display_id);

    if (display_id == 0) {
        lockdockd_set_error(error, error_size,
                            "Display token could not be resolved");
        return false;
    }

    if (bounds.size.width == 0 || bounds.size.height == 0) {
        snprintf(error, error_size, "Display %u not found", display_id);
        return false;
    }

    return true;
}

bool lockdockd_query_status(LockDockdStatus *status,
                            char *error,
                            size_t error_size) {
    CGDirectDisplayID dock_display;

    if (status == NULL) {
        lockdockd_set_error(error, error_size, "Internal error");
        return false;
    }

    memset(status, 0, sizeof(*status));
    status->location_index = -1;
    status->display_count =
        lockdockd_get_active_displays(status->displays, LOCKDOCKD_MAX_DISPLAYS);

    dock_display = lockdockd_get_dock_display();
    if (dock_display == 0) {
        if (lockdockd_is_accessibility_trusted()) {
            lockdockd_set_error(error, error_size,
                                "Could not determine current Dock display");
        } else {
            lockdockd_set_error(error, error_size,
                                "Could not determine current Dock display "
                                "(Accessibility permission is not granted)");
        }
        return false;
    }

    status->location_index =
        lockdockd_status_index_for_display(status, dock_display);
    if (status->location_index < 0) {
        snprintf(error, error_size,
                 "Dock display %u is not part of the active display list",
                 dock_display);
        return false;
    }

    return true;
}

int lockdockd_status_index_for_display(const LockDockdStatus *status,
                                       CGDirectDisplayID display_id) {
    if (status == NULL) {
        return -1;
    }

    for (uint32_t i = 0; i < status->display_count; i++) {
        if (status->displays[i] == display_id) {
            return (int)i;
        }
    }

    return -1;
}

static CGPoint lockdockd_current_mouse_location(void) {
    CGEventRef event = CGEventCreate(NULL);
    CGPoint point = CGPointMake(0, 0);

    if (event == NULL) {
        return point;
    }

    point = CGEventGetLocation(event);
    CFRelease(event);
    return point;
}

static void lockdockd_post_move_event(CGEventSourceRef source, CGPoint point) {
    CGEventRef event = CGEventCreateMouseEvent(source, kCGEventMouseMoved, point,
                                               kCGMouseButtonLeft);

    if (event == NULL) {
        return;
    }

    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

static bool lockdockd_set_cursor_association(bool associated) {
    return CGAssociateMouseAndMouseCursorPosition(associated) == kCGErrorSuccess;
}

static void lockdockd_post_edge_nudge(CGEventSourceRef source,
                                      CGPoint point,
                                      LockDockdDockOrientation orientation) {
    CGEventRef event = CGEventCreateMouseEvent(source, kCGEventMouseMoved, point,
                                               kCGMouseButtonLeft);

    if (event == NULL) {
        return;
    }

    if (orientation == LOCKDOCKD_ORIENT_BOTTOM) {
        CGEventSetIntegerValueField(event, kCGMouseEventDeltaY, 1);
    } else if (orientation == LOCKDOCKD_ORIENT_LEFT) {
        CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, -1);
    } else if (orientation == LOCKDOCKD_ORIENT_RIGHT) {
        CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, 1);
    }

    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

static bool lockdockd_probe_dock_display(CGDirectDisplayID display_id,
                                         LockDockdDockProbe *probe_out) {
    LockDockdDockProbe probe;

    lockdockd_reset_dock_probe(&probe);
    lockdockd_capture_dock_probe(&probe);

    if (probe_out != NULL) {
        *probe_out = probe;
    }

    return lockdockd_resolve_dock_probe(&probe, false) == display_id;
}

static bool lockdockd_wait_for_dock_relocation(CGEventSourceRef source,
                                               CGPoint edge,
                                               LockDockdDockOrientation orientation,
                                               CGDirectDisplayID display_id,
                                               LockDockdDockProbe *probe_out) {
    LockDockdDockProbe probe;

    lockdockd_reset_dock_probe(&probe);

    for (int i = 0; i < 60; i++) {
        lockdockd_post_edge_nudge(source, edge, orientation);
        usleep(15 * 1000);

        if (((i + 1) % 3) == 0 && lockdockd_probe_dock_display(display_id, &probe)) {
            if (probe_out != NULL) {
                *probe_out = probe;
            }

            return true;
        }
    }

    for (int i = 0; i < 8; i++) {
        if (lockdockd_probe_dock_display(display_id, &probe)) {
            if (probe_out != NULL) {
                *probe_out = probe;
            }

            return true;
        }

        usleep(10 * 1000);
    }

    if (probe_out != NULL) {
        *probe_out = probe;
    }

    return false;
}

static void lockdockd_smooth_move(CGEventSourceRef source,
                                  CGPoint from,
                                  CGPoint to,
                                  int steps,
                                  useconds_t delay_us) {
    for (int step = 1; step <= steps; step++) {
        CGFloat t = (CGFloat)step / (CGFloat)steps;
        CGPoint point =
            CGPointMake(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t);

        CGWarpMouseCursorPosition(point);
        lockdockd_post_move_event(source, point);
        usleep(delay_us);
    }
}

bool lockdockd_relocate_display(CGDirectDisplayID display_id,
                                char *error,
                                size_t error_size) {
    CGRect bounds;
    LockDockdDockOrientation orientation;
    CGPoint old_position;
    bool cursor_locked = false;
    bool success = false;
    CGEventSourceRef source = NULL;
    LockDockdSafeSegment safe_segment;
    LockDockdDockProbe dock_probe;
    CGPoint approach = CGPointZero;
    CGPoint edge = CGPointZero;
    CGDirectDisplayID new_display;
    bool relocated_via_fast_probe = false;

    if (!lockdockd_validate_display(display_id, error, error_size)) {
        return false;
    }

    bounds = CGDisplayBounds(display_id);
    lockdockd_invalidate_dock_orientation_cache();
    orientation = lockdockd_get_dock_orientation();
    old_position = lockdockd_current_mouse_location();
    lockdockd_reset_dock_probe(&dock_probe);

    source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    if (source != NULL) {
        CGEventSourceSetLocalEventsSuppressionInterval(source, 0.0);
    }

    cursor_locked = lockdockd_set_cursor_association(false);
    safe_segment = lockdockd_find_safe_edge_segment(display_id, orientation);

    const CGFloat edge_offset = 1.0;

    switch (orientation) {
        case LOCKDOCKD_ORIENT_BOTTOM: {
            CGFloat edge_y = bounds.origin.y + bounds.size.height;
            CGFloat trigger_x = safe_segment.center;

            approach = CGPointMake(trigger_x, edge_y - edge_offset);
            edge = CGPointMake(trigger_x, edge_y - 1.0);
            break;
        }

        case LOCKDOCKD_ORIENT_LEFT: {
            CGFloat edge_x = bounds.origin.x;
            CGFloat trigger_y = safe_segment.center;

            approach = CGPointMake(edge_x + edge_offset, trigger_y);
            edge = CGPointMake(edge_x + 1.0, trigger_y);
            break;
        }

        case LOCKDOCKD_ORIENT_RIGHT: {
            CGFloat edge_x = bounds.origin.x + bounds.size.width;
            CGFloat trigger_y = safe_segment.center;

            approach = CGPointMake(edge_x - edge_offset, trigger_y);
            edge = CGPointMake(edge_x - 1.0, trigger_y);
            break;
        }
    }

    CGWarpMouseCursorPosition(approach);
    lockdockd_post_move_event(source, approach);
    usleep(30 * 1000);

    lockdockd_smooth_move(source, approach, edge, 10, 15 * 1000);
    relocated_via_fast_probe = lockdockd_wait_for_dock_relocation(
        source, edge, orientation, display_id, &dock_probe);

    if (!relocated_via_fast_probe) {
        lockdockd_capture_dock_probe(&dock_probe);
    }

    new_display = lockdockd_resolve_dock_probe(&dock_probe, true);
    if (new_display == display_id) {
        success = true;
    } else if (new_display == 0) {
        if (lockdockd_is_accessibility_trusted()) {
            lockdockd_set_error(error, error_size,
                                "Could not determine current Dock display");
        } else {
            lockdockd_set_error(error, error_size,
                                "Could not determine current Dock display "
                                "(Accessibility permission is not granted)");
        }
    } else {
        snprintf(error, error_size, "Dock is on display %u (expected %u)",
                 new_display, display_id);
    }

    CGWarpMouseCursorPosition(old_position);
    if (cursor_locked) {
        lockdockd_set_cursor_association(true);
    }

    if (source != NULL) {
        CFRelease(source);
    }

    return success;
}
