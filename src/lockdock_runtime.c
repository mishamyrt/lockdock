#include "lockdock_runtime.h"

#include "lockdock_display.h"
#include "lockdock_platform.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const int LOCKDOCK_RELOCATION_NUDGE_ATTEMPTS = 60;
static const int LOCKDOCK_RELOCATION_PROBE_INTERVAL = 3;
static const useconds_t LOCKDOCK_RELOCATION_NUDGE_DELAY_US = 15000;
static const int LOCKDOCK_RELOCATION_VERIFY_ATTEMPTS = 8;
static const useconds_t LOCKDOCK_RELOCATION_VERIFY_DELAY_US = 10000;
static const useconds_t LOCKDOCK_RELOCATION_APPROACH_DELAY_US = 30000;
static const int LOCKDOCK_RELOCATION_EDGE_MOVE_STEPS = 10;
static const useconds_t LOCKDOCK_RELOCATION_EDGE_MOVE_DELAY_US = 15000;

static void lockdock_set_error(char *buffer,
                               size_t buffer_size,
                               const char *message) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", message);
}

bool lockdock_copy_display_label(CGDirectDisplayID display_id,
                                 char *buffer,
                                 size_t buffer_size) {
    if (buffer == NULL || buffer_size == 0) {
        return false;
    }

    if (CGDisplayIsBuiltin(display_id)) {
        snprintf(buffer, buffer_size, "Built-in Display");
        return true;
    }

    if (lockdock_copy_display_name(display_id, buffer, buffer_size)) {
        return true;
    }

    snprintf(buffer, buffer_size, "Display-%u", display_id);
    return true;
}

static bool lockdock_validate_display(CGDirectDisplayID display_id,
                                      char *error,
                                      size_t error_size) {
    CGRect bounds = CGDisplayBounds(display_id);

    if (display_id == 0) {
        lockdock_set_error(error, error_size, "Display token could not be resolved");
        return false;
    }

    if (bounds.size.width == 0 || bounds.size.height == 0) {
        snprintf(error, error_size, "Display %u not found", display_id);
        return false;
    }

    return true;
}

bool lockdock_query_status(LockDockStatus *status, char *error, size_t error_size) {
    CGDirectDisplayID dock_display;

    if (status == NULL) {
        lockdock_set_error(error, error_size, "Internal error");
        return false;
    }

    memset(status, 0, sizeof(*status));
    status->location_index = -1;
    status->display_count =
        lockdock_get_active_displays(status->displays, LOCKDOCK_MAX_DISPLAYS);

    dock_display = lockdock_get_dock_display();
    if (dock_display == 0) {
        if (lockdock_is_accessibility_trusted()) {
            lockdock_set_error(error, error_size,
                               "Could not determine current Dock display");
        } else {
            lockdock_set_error(error, error_size,
                               "Could not determine current Dock display "
                               "(Accessibility permission is not granted)");
        }
        return false;
    }

    status->location_index = lockdock_status_index_for_display(status, dock_display);
    if (status->location_index < 0) {
        snprintf(error, error_size,
                 "Dock display %u is not part of the active display list",
                 dock_display);
        return false;
    }

    return true;
}

int lockdock_status_index_for_display(const LockDockStatus *status,
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

static CGPoint lockdock_current_mouse_location(void) {
    CGEventRef event = CGEventCreate(NULL);
    CGPoint point = CGPointMake(0, 0);

    if (event == NULL) {
        return point;
    }

    point = CGEventGetLocation(event);
    CFRelease(event);
    return point;
}

static void lockdock_post_move_event(CGEventSourceRef source, CGPoint point) {
    CGEventRef event = CGEventCreateMouseEvent(source, kCGEventMouseMoved, point,
                                               kCGMouseButtonLeft);

    if (event == NULL) {
        return;
    }

    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

static bool lockdock_set_cursor_association(bool associated) {
    return CGAssociateMouseAndMouseCursorPosition(associated) == kCGErrorSuccess;
}

static void lockdock_post_edge_nudge(CGEventSourceRef source,
                                     CGPoint point,
                                     LockDockDockOrientation orientation) {
    CGEventRef event = CGEventCreateMouseEvent(source, kCGEventMouseMoved, point,
                                               kCGMouseButtonLeft);

    if (event == NULL) {
        return;
    }

    if (orientation == LOCKDOCK_ORIENT_BOTTOM) {
        CGEventSetIntegerValueField(event, kCGMouseEventDeltaY, 1);
    } else if (orientation == LOCKDOCK_ORIENT_LEFT) {
        CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, -1);
    } else if (orientation == LOCKDOCK_ORIENT_RIGHT) {
        CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, 1);
    }

    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

static bool lockdock_probe_dock_display(CGDirectDisplayID display_id,
                                        LockDockDockProbe *probe_out) {
    LockDockDockProbe probe;

    lockdock_reset_dock_probe(&probe);
    lockdock_capture_dock_probe(&probe);

    if (probe_out != NULL) {
        *probe_out = probe;
    }

    return lockdock_resolve_dock_probe(&probe, false) == display_id;
}

static bool lockdock_wait_for_dock_relocation(CGEventSourceRef source,
                                              CGPoint edge,
                                              LockDockDockOrientation orientation,
                                              CGDirectDisplayID display_id,
                                              LockDockDockProbe *probe_out) {
    LockDockDockProbe probe;

    lockdock_reset_dock_probe(&probe);

    for (int i = 0; i < LOCKDOCK_RELOCATION_NUDGE_ATTEMPTS; i++) {
        lockdock_post_edge_nudge(source, edge, orientation);
        usleep(LOCKDOCK_RELOCATION_NUDGE_DELAY_US);

        if (((i + 1) % LOCKDOCK_RELOCATION_PROBE_INTERVAL) == 0 &&
            lockdock_probe_dock_display(display_id, &probe)) {
            if (probe_out != NULL) {
                *probe_out = probe;
            }

            return true;
        }
    }

    for (int i = 0; i < LOCKDOCK_RELOCATION_VERIFY_ATTEMPTS; i++) {
        if (lockdock_probe_dock_display(display_id, &probe)) {
            if (probe_out != NULL) {
                *probe_out = probe;
            }

            return true;
        }

        usleep(LOCKDOCK_RELOCATION_VERIFY_DELAY_US);
    }

    if (probe_out != NULL) {
        *probe_out = probe;
    }

    return false;
}

static void lockdock_smooth_move(CGEventSourceRef source,
                                 CGPoint from,
                                 CGPoint to,
                                 int steps,
                                 useconds_t delay_us) {
    for (int step = 1; step <= steps; step++) {
        CGFloat progress = (CGFloat)step / (CGFloat)steps;
        CGPoint point = CGPointMake(from.x + ((to.x - from.x) * progress),
                                    from.y + ((to.y - from.y) * progress));

        CGWarpMouseCursorPosition(point);
        lockdock_post_move_event(source, point);
        usleep(delay_us);
    }
}

static CGFloat lockdock_choose_trigger_coordinate(
    CGDirectDisplayID target_display_id,
    LockDockDockOrientation orientation,
    LockDockSafeSegment safe_segment) {
    CGRect bounds = CGDisplayBounds(target_display_id);
    CGFloat preferred_coordinate;

    if (orientation == LOCKDOCK_ORIENT_BOTTOM) {
        preferred_coordinate = bounds.origin.x + bounds.size.width - 10.0;
        if (preferred_coordinate < bounds.origin.x) {
            preferred_coordinate = bounds.origin.x;
        }
    } else {
        preferred_coordinate = bounds.origin.y + bounds.size.height - 10.0;
        if (preferred_coordinate < bounds.origin.y) {
            preferred_coordinate = bounds.origin.y;
        }
    }

    if (!lockdock_edge_point_has_contact(target_display_id, orientation,
                                         preferred_coordinate)) {
        return preferred_coordinate;
    }

    return safe_segment.center;
}

bool lockdock_relocate_display(CGDirectDisplayID display_id,
                               char *error,
                               size_t error_size) {
    CGRect bounds;
    LockDockDockOrientation orientation;
    CGPoint old_position;
    bool cursor_locked = false;
    bool success = false;
    CGEventSourceRef source = NULL;
    LockDockSafeSegment safe_segment;
    LockDockDockProbe dock_probe;
    CGPoint approach = CGPointZero;
    CGPoint edge = CGPointZero;
    CGDirectDisplayID new_display;
    bool relocated_via_fast_probe = false;

    if (!lockdock_validate_display(display_id, error, error_size)) {
        return false;
    }

    bounds = CGDisplayBounds(display_id);
    lockdock_invalidate_dock_orientation_cache();
    orientation = lockdock_get_dock_orientation();
    old_position = lockdock_current_mouse_location();
    lockdock_reset_dock_probe(&dock_probe);

    source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    if (source != NULL) {
        CGEventSourceSetLocalEventsSuppressionInterval(source, 0.0);
    }

    cursor_locked = lockdock_set_cursor_association(false);
    safe_segment = lockdock_find_safe_edge_segment(display_id, orientation);

    const CGFloat edge_offset = 1.0;

    switch (orientation) {
        case LOCKDOCK_ORIENT_BOTTOM: {
            CGFloat edge_y = bounds.origin.y + bounds.size.height;
            CGFloat trigger_x = lockdock_choose_trigger_coordinate(
                display_id, orientation, safe_segment);

            approach = CGPointMake(trigger_x, edge_y - edge_offset);
            edge = CGPointMake(trigger_x, edge_y - 1.0);
            break;
        }

        case LOCKDOCK_ORIENT_LEFT: {
            CGFloat edge_x = bounds.origin.x;
            CGFloat trigger_y = lockdock_choose_trigger_coordinate(
                display_id, orientation, safe_segment);

            approach = CGPointMake(edge_x + edge_offset, trigger_y);
            edge = CGPointMake(edge_x + 1.0, trigger_y);
            break;
        }

        case LOCKDOCK_ORIENT_RIGHT: {
            CGFloat edge_x = bounds.origin.x + bounds.size.width;
            CGFloat trigger_y = lockdock_choose_trigger_coordinate(
                display_id, orientation, safe_segment);

            approach = CGPointMake(edge_x - edge_offset, trigger_y);
            edge = CGPointMake(edge_x - 1.0, trigger_y);
            break;
        }
    }

    CGWarpMouseCursorPosition(approach);
    lockdock_post_move_event(source, approach);
    usleep(LOCKDOCK_RELOCATION_APPROACH_DELAY_US);

    lockdock_smooth_move(source, approach, edge, LOCKDOCK_RELOCATION_EDGE_MOVE_STEPS,
                         LOCKDOCK_RELOCATION_EDGE_MOVE_DELAY_US);
    relocated_via_fast_probe = lockdock_wait_for_dock_relocation(
        source, edge, orientation, display_id, &dock_probe);

    if (!relocated_via_fast_probe) {
        lockdock_capture_dock_probe(&dock_probe);
    }

    new_display = lockdock_resolve_dock_probe(&dock_probe, true);
    if (new_display == display_id) {
        success = true;
    } else if (new_display == 0) {
        if (lockdock_is_accessibility_trusted()) {
            lockdock_set_error(error, error_size,
                               "Could not determine current Dock display");
        } else {
            lockdock_set_error(error, error_size,
                               "Could not determine current Dock display "
                               "(Accessibility permission is not granted)");
        }
    } else {
        snprintf(error, error_size, "Dock is on display %u (expected %u)",
                 new_display, display_id);
    }

    CGWarpMouseCursorPosition(old_position);
    if (cursor_locked) {
        lockdock_set_cursor_association(true);
    }

    if (source != NULL) {
        CFRelease(source);
    }

    return success;
}
