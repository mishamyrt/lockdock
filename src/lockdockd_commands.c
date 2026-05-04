#include "lockdockd_commands.h"

#include "lockdockd_display.h"
#include "lockdockd_objc_bridge.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#define LOCKDOCKD_MAX_DISPLAYS 32
#define LOCKDOCKD_DISPLAY_NAME_BUFFER_SIZE 256

static CGDirectDisplayID g_locked_display = 0;
static volatile int g_locker_running = 1;
static double g_lock_edge_zone = 4.0;

static const char *lockdockd_orientation_name(
    LockDockdDockOrientation orientation) {
    static const char *const names[] = {"bottom", "left", "right"};

    return names[orientation];
}

static void lockdockd_print_display_name(CGDirectDisplayID display_id) {
    char name[LOCKDOCKD_DISPLAY_NAME_BUFFER_SIZE];

    if (lockdockd_copy_display_name(display_id, name, sizeof(name))) {
        printf("%s", name);
        return;
    }

    if (CGDisplayIsBuiltin(display_id)) {
        printf("Built-in Display");
        return;
    }

    printf("Display-%u", display_id);
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

static void lockdockd_smooth_move(CGEventSourceRef source,
                                  CGPoint from,
                                  CGPoint to,
                                  int steps,
                                  useconds_t delay_us) {
    for (int step = 1; step <= steps; step++) {
        CGFloat t = (CGFloat)step / (CGFloat)steps;
        CGPoint point = CGPointMake(from.x + (to.x - from.x) * t,
                                    from.y + (to.y - from.y) * t);

        CGWarpMouseCursorPosition(point);
        lockdockd_post_move_event(source, point);
        usleep(delay_us);
    }
}

static void lockdockd_signal_handler(int signal_number) {
    if (signal_number == SIGINT || signal_number == SIGTERM) {
        g_locker_running = 0;
    }
}

static CGEventRef lockdockd_locker_event_callback(CGEventTapProxy proxy,
                                                  CGEventType type,
                                                  CGEventRef event,
                                                  void *user_info) {
    CGPoint point;
    CGDirectDisplayID current_display;
    CGRect bounds;
    CGFloat distance_from_bottom;

    (void)proxy;
    (void)user_info;

    if (type != kCGEventMouseMoved && type != kCGEventLeftMouseDragged &&
        type != kCGEventRightMouseDragged &&
        type != kCGEventOtherMouseDragged) {
        return event;
    }

    point = CGEventGetLocation(event);
    current_display = lockdockd_find_display_at_point(point);
    if (current_display == 0 || current_display == g_locked_display) {
        return event;
    }

    bounds = CGDisplayBounds(current_display);
    distance_from_bottom = (bounds.origin.y + bounds.size.height) - point.y;

    if (distance_from_bottom < 0 || distance_from_bottom > g_lock_edge_zone) {
        return event;
    }

    return NULL;
}

int lockdockd_cmd_list(void) {
    CGDirectDisplayID displays[LOCKDOCKD_MAX_DISPLAYS];
    uint32_t count = 0;
    CGDirectDisplayID main_display_id = CGMainDisplayID();

    CGGetActiveDisplayList(LOCKDOCKD_MAX_DISPLAYS, displays, &count);

    for (uint32_t i = 0; i < count; i++) {
        CGRect bounds = CGDisplayBounds(displays[i]);

        printf("[%u] id=%u ", i, displays[i]);
        lockdockd_print_display_name(displays[i]);
        printf(" %.0fx%.0f@(%.0f,%.0f)%s%s\n", bounds.size.width,
               bounds.size.height, bounds.origin.x, bounds.origin.y,
               CGDisplayIsBuiltin(displays[i]) ? " builtin" : "",
               displays[i] == main_display_id ? " primary" : "");
    }

    return 0;
}

int lockdockd_cmd_relocate(const char *display_arg) {
    CGDirectDisplayID display_id = lockdockd_resolve_display_arg(display_arg);
    CGRect bounds;
    LockDockdDockOrientation orientation;
    CGPoint old_position;
    CGEventSourceRef source;
    LockDockdSafeSegment safe_segment;
    CGPoint approach = CGPointZero;
    CGPoint edge = CGPointZero;

    if (display_id == 0) {
        fprintf(stderr,
                "Cannot resolve display '%s'. Use index or display ID from 'list'\n",
                display_arg);
        return 1;
    }

    bounds = CGDisplayBounds(display_id);
    if (bounds.size.width == 0 || bounds.size.height == 0) {
        fprintf(stderr, "Display %u not found or has zero size\n", display_id);
        return 1;
    }

    printf("Relocating dock to display %u (%.0fx%.0f)\n", display_id,
           bounds.size.width, bounds.size.height);

    orientation = lockdockd_get_dock_orientation();
    printf("Dock orientation: %s\n", lockdockd_orientation_name(orientation));

    old_position = lockdockd_current_mouse_location();
    printf("Current cursor: (%.0f, %.0f)\n", old_position.x, old_position.y);

    source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    if (source != NULL) {
        CGEventSourceSetLocalEventsSuppressionInterval(source, 0.0);
    }

    safe_segment = lockdockd_find_safe_edge_segment(display_id, orientation);
    printf("Safe edge segment: %.0f..%.0f (width=%.0f, center=%.0f)\n",
           safe_segment.start, safe_segment.end, safe_segment.width,
           safe_segment.center);

    switch (orientation) {
        case LOCKDOCKD_ORIENT_BOTTOM: {
            CGFloat edge_y = bounds.origin.y + bounds.size.height;
            CGFloat trigger_x = safe_segment.center;

            approach = CGPointMake(trigger_x, edge_y - 50.0);
            edge = CGPointMake(trigger_x, edge_y - 1.0);

            if (fabs(old_position.x - trigger_x) > 50.0) {
                CGPoint horizontal_target = CGPointMake(trigger_x, old_position.y);
                lockdockd_smooth_move(source, old_position, horizontal_target, 8,
                                      12 * 1000);
            }
            break;
        }

        case LOCKDOCKD_ORIENT_LEFT: {
            CGFloat edge_x = bounds.origin.x;
            CGFloat trigger_y = safe_segment.center;

            approach = CGPointMake(edge_x + 50.0, trigger_y);
            edge = CGPointMake(edge_x + 1.0, trigger_y);

            if (fabs(old_position.y - trigger_y) > 50.0) {
                CGPoint vertical_target = CGPointMake(old_position.x, trigger_y);
                lockdockd_smooth_move(source, old_position, vertical_target, 8,
                                      12 * 1000);
            }
            break;
        }

        case LOCKDOCKD_ORIENT_RIGHT: {
            CGFloat edge_x = bounds.origin.x + bounds.size.width;
            CGFloat trigger_y = safe_segment.center;

            approach = CGPointMake(edge_x - 50.0, trigger_y);
            edge = CGPointMake(edge_x - 1.0, trigger_y);

            if (fabs(old_position.y - trigger_y) > 50.0) {
                CGPoint vertical_target = CGPointMake(old_position.x, trigger_y);
                lockdockd_smooth_move(source, old_position, vertical_target, 8,
                                      12 * 1000);
            }
            break;
        }
    }

    CGWarpMouseCursorPosition(approach);
    lockdockd_post_move_event(source, approach);
    usleep(30 * 1000);

    lockdockd_smooth_move(source, approach, edge, 10, 15 * 1000);

    for (int i = 0; i < 60; i++) {
        CGEventRef event = CGEventCreateMouseEvent(source, kCGEventMouseMoved, edge,
                                                   kCGMouseButtonLeft);

        if (event != NULL) {
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

        usleep(15 * 1000);
    }

    usleep(400 * 1000);

    if (source != NULL) {
        CFRelease(source);
    }

    CGWarpMouseCursorPosition(old_position);

    {
        CGDirectDisplayID new_display = lockdockd_get_dock_display();

        if (new_display == display_id) {
            printf("Dock successfully moved to display %u\n", display_id);
        } else if (new_display != 0) {
            printf("Dock is on display %u (expected %u)\n", new_display,
                   display_id);
        } else {
            printf("Could not determine current Dock display\n");
        }
    }

    return 0;
}

int lockdockd_cmd_lock(const char *display_arg) {
    CGDirectDisplayID display_id = lockdockd_resolve_display_arg(display_arg);
    CGRect bounds;
    CGEventMask mask;
    CFMachPortRef tap;
    CFRunLoopSourceRef source;

    if (display_id == 0) {
        fprintf(stderr,
                "Cannot resolve display '%s'. Use index or display ID from 'list'\n",
                display_arg);
        return 1;
    }

    bounds = CGDisplayBounds(display_id);
    if (bounds.size.width == 0 || bounds.size.height == 0) {
        fprintf(stderr, "Display %u not found\n", display_id);
        return 1;
    }

    g_locked_display = display_id;
    g_locker_running = 1;

    printf("Locking Dock to display %u ", display_id);
    lockdockd_print_display_name(display_id);
    printf(" (%.0fx%.0f)\n", bounds.size.width, bounds.size.height);
    printf("Edge zone: %.0fpx. Press Ctrl+C to stop.\n", g_lock_edge_zone);

    signal(SIGINT, lockdockd_signal_handler);
    signal(SIGTERM, lockdockd_signal_handler);

    mask = CGEventMaskBit(kCGEventMouseMoved) |
           CGEventMaskBit(kCGEventLeftMouseDragged) |
           CGEventMaskBit(kCGEventRightMouseDragged) |
           CGEventMaskBit(kCGEventOtherMouseDragged);

    tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                           kCGEventTapOptionDefault, mask,
                           lockdockd_locker_event_callback, NULL);
    if (tap == NULL) {
        fprintf(stderr, "Failed to create event tap.\n");
        fprintf(stderr, "Grant Accessibility permission in System Settings\n");
        return 1;
    }

    source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
    CGEventTapEnable(tap, true);

    printf("Event tap active. Locking is on.\n");

    while (g_locker_running) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
    }

    printf("\nStopping lock. Restoring normal behavior.\n");

    CGEventTapEnable(tap, false);
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
    CFRelease(source);
    CFRelease(tap);

    return 0;
}

int lockdockd_cmd_status(void) {
    CGDirectDisplayID dock_display = lockdockd_get_dock_display();
    CGRect bounds;

    if (dock_display == 0) {
        printf("Could not determine current Dock display\n");
        return 1;
    }

    bounds = CGDisplayBounds(dock_display);
    printf("Dock is on display %u ", dock_display);
    lockdockd_print_display_name(dock_display);
    printf(" (%.0fx%.0f@%.0f,%.0f)\n", bounds.size.width, bounds.size.height,
           bounds.origin.x, bounds.origin.y);
    return 0;
}

void lockdockd_print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s list                     List all displays\n", prog);
    printf("  %s status                   Show which display the Dock is on\n",
           prog);
    printf("  %s relocate <display-id>    Move Dock to a display (via safe edge "
           "zone)\n",
           prog);
    printf("  %s lock <display-id>        Lock Dock to a display (block edge\n",
           prog);
    printf("                              pressure on other displays)\n");
}
