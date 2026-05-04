#include "lockdockd_locker.h"

#include "lockdockd_display.h"
#include "lockdockd_platform.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stdio.h>

static CGDirectDisplayID g_locked_display = 0;
static double g_lock_edge_zone = 4.0;
static CFMachPortRef g_event_tap = NULL;
static CFRunLoopSourceRef g_event_source = NULL;

static CGFloat lockdockd_distance_from_dock_edge(
    CGPoint point,
    CGRect bounds,
    LockDockdDockOrientation orientation) {
    if (orientation == LOCKDOCKD_ORIENT_LEFT) {
        return point.x - bounds.origin.x;
    }

    if (orientation == LOCKDOCKD_ORIENT_RIGHT) {
        return (bounds.origin.x + bounds.size.width) - point.x;
    }

    return (bounds.origin.y + bounds.size.height) - point.y;
}

static CGEventRef lockdockd_locker_event_callback(CGEventTapProxy proxy,
                                                  CGEventType type,
                                                  CGEventRef event,
                                                  void *user_info) {
    CGPoint point;
    CGDirectDisplayID current_display;
    CGRect bounds;
    CGFloat distance;
    LockDockdDockOrientation orientation;

    (void)proxy;
    (void)user_info;

    if (type != kCGEventMouseMoved && type != kCGEventLeftMouseDragged &&
        type != kCGEventRightMouseDragged && type != kCGEventOtherMouseDragged) {
        return event;
    }

    if (g_locked_display == 0) {
        return event;
    }

    point = CGEventGetLocation(event);
    current_display = lockdockd_find_display_at_point(point);
    if (current_display == 0 || current_display == g_locked_display) {
        return event;
    }

    bounds = CGDisplayBounds(current_display);
    orientation = lockdockd_get_dock_orientation();
    distance = lockdockd_distance_from_dock_edge(point, bounds, orientation);

    if (distance < 0 || distance > g_lock_edge_zone) {
        return event;
    }

    return NULL;
}

static void lockdockd_set_error(char *buffer,
                                size_t buffer_size,
                                const char *message) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", message);
}

static void lockdockd_locker_release_tap(void) {
    if (g_event_tap != NULL) {
        CGEventTapEnable(g_event_tap, false);
    }

    if (g_event_source != NULL) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), g_event_source,
                              kCFRunLoopCommonModes);
        CFRelease(g_event_source);
        g_event_source = NULL;
    }

    if (g_event_tap != NULL) {
        CFRelease(g_event_tap);
        g_event_tap = NULL;
    }
}

static bool lockdockd_locker_ensure_tap(char *error, size_t error_size) {
    CGEventMask mask;

    if (g_event_tap != NULL && g_event_source != NULL) {
        return true;
    }

    mask = CGEventMaskBit(kCGEventMouseMoved) |
           CGEventMaskBit(kCGEventLeftMouseDragged) |
           CGEventMaskBit(kCGEventRightMouseDragged) |
           CGEventMaskBit(kCGEventOtherMouseDragged);

    g_event_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                   kCGEventTapOptionDefault, mask,
                                   lockdockd_locker_event_callback, NULL);
    if (g_event_tap == NULL) {
        lockdockd_set_error(error, error_size,
                            "Failed to create event tap. Grant Accessibility "
                            "permission in System Settings");
        return false;
    }

    g_event_source =
        CFMachPortCreateRunLoopSource(kCFAllocatorDefault, g_event_tap, 0);
    if (g_event_source == NULL) {
        lockdockd_locker_release_tap();
        lockdockd_set_error(error, error_size, "Failed to create event tap source");
        return false;
    }

    CFRunLoopAddSource(CFRunLoopGetCurrent(), g_event_source, kCFRunLoopCommonModes);
    CGEventTapEnable(g_event_tap, true);
    return true;
}

bool lockdockd_locker_set_target(CGDirectDisplayID display_id,
                                 char *error,
                                 size_t error_size) {
    if (!lockdockd_locker_ensure_tap(error, error_size)) {
        return false;
    }

    g_locked_display = display_id;
    return true;
}

void lockdockd_locker_clear_target(void) {
    g_locked_display = 0;
    lockdockd_locker_release_tap();
}

CGDirectDisplayID lockdockd_locker_get_target(void) {
    return g_locked_display;
}

void lockdockd_locker_shutdown(void) {
    lockdockd_locker_clear_target();
}
