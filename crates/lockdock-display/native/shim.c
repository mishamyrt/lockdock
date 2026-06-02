#include "shim.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define LOCKDOCK_SHIM_MAX_WINDOWS 256

static CFMachPortRef g_event_tap = NULL;
static CFRunLoopSourceRef g_event_source = NULL;
static CFRunLoopRef g_event_run_loop = NULL;
static pthread_t g_event_thread;
static pthread_mutex_t g_event_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_event_cond = PTHREAD_COND_INITIALIZER;
static bool g_event_thread_starting = false;
static bool g_event_thread_running = false;
static bool g_event_thread_joinable = false;
static char g_event_error[256];

static LockDockShimRect lockdock_shim_rect_from_cg(CGRect rect) {
    LockDockShimRect out = {rect.origin.x, rect.origin.y, rect.size.width,
                            rect.size.height};
    return out;
}

static CGRect lockdock_shim_rect_to_cg(LockDockShimRect rect) {
    return CGRectMake(rect.x, rect.y, rect.width, rect.height);
}

uint32_t lockdock_shim_get_active_displays(uint32_t *displays,
                                           uint32_t max_displays) {
    uint32_t count = 0;

    if (displays == NULL || max_displays == 0) {
        return 0;
    }

    CGGetActiveDisplayList(max_displays, displays, &count);
    return count;
}

bool lockdock_shim_copy_display_bounds(uint32_t display_id,
                                       LockDockShimRect *rect_out) {
    CGRect bounds;

    if (display_id == 0 || rect_out == NULL) {
        return false;
    }

    bounds = CGDisplayBounds(display_id);
    if (CGRectIsNull(bounds) || CGRectIsEmpty(bounds)) {
        return false;
    }

    *rect_out = lockdock_shim_rect_from_cg(bounds);
    return true;
}

bool lockdock_shim_copy_display_uuid(uint32_t display_id,
                                     char *buffer,
                                     size_t buffer_size) {
    CFUUIDRef uuid = NULL;
    CFStringRef uuid_string = NULL;
    bool copied = false;

    if (display_id == 0 || buffer == NULL || buffer_size == 0) {
        return false;
    }

    buffer[0] = '\0';
    uuid = CGDisplayCreateUUIDFromDisplayID(display_id);
    if (uuid == NULL) {
        return false;
    }

    uuid_string = CFUUIDCreateString(kCFAllocatorDefault, uuid);
    if (uuid_string != NULL) {
        copied = CFStringGetCString(uuid_string, buffer, buffer_size,
                                    kCFStringEncodingUTF8);
        CFRelease(uuid_string);
    }

    CFRelease(uuid);
    return copied;
}

bool lockdock_shim_display_is_builtin(uint32_t display_id) {
    return CGDisplayIsBuiltin(display_id) != 0;
}

uint32_t lockdock_shim_display_vendor_number(uint32_t display_id) {
    return CGDisplayVendorNumber(display_id);
}

uint32_t lockdock_shim_display_model_number(uint32_t display_id) {
    return CGDisplayModelNumber(display_id);
}

uint32_t lockdock_shim_display_serial_number(uint32_t display_id) {
    return CGDisplaySerialNumber(display_id);
}

bool lockdock_shim_is_accessibility_trusted(void) {
    return AXIsProcessTrusted();
}

static bool lockdock_shim_owner_is_dock(CFDictionaryRef window) {
    CFStringRef owner = (CFStringRef)CFDictionaryGetValue(window, kCGWindowOwnerName);

    return owner != NULL && CFGetTypeID(owner) == CFStringGetTypeID() &&
           CFStringCompare(owner, CFSTR("Dock"), 0) == kCFCompareEqualTo;
}

static bool lockdock_shim_window_is_wallpaper(CFDictionaryRef window) {
    CFStringRef name = (CFStringRef)CFDictionaryGetValue(window, kCGWindowName);

    return name != NULL && CFGetTypeID(name) == CFStringGetTypeID() &&
           CFStringHasPrefix(name, CFSTR("Wallpaper-"));
}

static bool lockdock_shim_copy_window_bounds(CFDictionaryRef window,
                                             CGRect *bounds_out) {
    CFDictionaryRef bounds =
        (CFDictionaryRef)CFDictionaryGetValue(window, kCGWindowBounds);

    return bounds != NULL && CFGetTypeID(bounds) == CFDictionaryGetTypeID() &&
           CGRectMakeWithDictionaryRepresentation(bounds, bounds_out) &&
           !CGRectIsNull(*bounds_out) && !CGRectIsEmpty(*bounds_out);
}

static bool lockdock_shim_copy_dock_window_bounds_for_option(
    CGWindowListOption option,
    LockDockShimRect *rect_out) {
    CFArrayRef windows = CGWindowListCopyWindowInfo(option, kCGNullWindowID);
    CGRect best_bounds = CGRectZero;
    double best_area = 0;
    int best_layer = INT_MIN;

    if (windows == NULL) {
        return false;
    }

    for (CFIndex i = 0; i < CFArrayGetCount(windows); i++) {
        CFDictionaryRef window =
            (CFDictionaryRef)CFArrayGetValueAtIndex(windows, i);
        CGRect bounds = CGRectZero;
        int layer = INT_MIN;
        CFNumberRef layer_number;
        double area;

        if (window == NULL || CFGetTypeID(window) != CFDictionaryGetTypeID() ||
            !lockdock_shim_owner_is_dock(window) ||
            lockdock_shim_window_is_wallpaper(window) ||
            !lockdock_shim_copy_window_bounds(window, &bounds)) {
            continue;
        }

        layer_number = (CFNumberRef)CFDictionaryGetValue(window, kCGWindowLayer);
        if (layer_number != NULL && CFGetTypeID(layer_number) == CFNumberGetTypeID()) {
            CFNumberGetValue(layer_number, kCFNumberIntType, &layer);
        }

        area = bounds.size.width * bounds.size.height;
        if (layer > best_layer || (layer == best_layer && area > best_area)) {
            best_layer = layer;
            best_area = area;
            best_bounds = bounds;
        }
    }

    CFRelease(windows);

    if (CGRectIsNull(best_bounds) || CGRectIsEmpty(best_bounds)) {
        return false;
    }

    *rect_out = lockdock_shim_rect_from_cg(best_bounds);
    return true;
}

bool lockdock_shim_copy_dock_window_bounds(LockDockShimRect *rect_out) {
    if (rect_out == NULL) {
        return false;
    }

    return lockdock_shim_copy_dock_window_bounds_for_option(
               kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
               rect_out) ||
           lockdock_shim_copy_dock_window_bounds_for_option(kCGWindowListOptionAll,
                                                            rect_out);
}

static pid_t lockdock_shim_find_dock_pid(void) {
    CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionAll,
                                                   kCGNullWindowID);

    if (windows == NULL) {
        return 0;
    }

    for (CFIndex i = 0; i < CFArrayGetCount(windows); i++) {
        CFDictionaryRef window =
            (CFDictionaryRef)CFArrayGetValueAtIndex(windows, i);
        CFNumberRef pid_number;
        int pid = 0;

        if (window == NULL || CFGetTypeID(window) != CFDictionaryGetTypeID() ||
            !lockdock_shim_owner_is_dock(window)) {
            continue;
        }

        pid_number = (CFNumberRef)CFDictionaryGetValue(window, kCGWindowOwnerPID);
        if (pid_number != NULL && CFGetTypeID(pid_number) == CFNumberGetTypeID() &&
            CFNumberGetValue(pid_number, kCFNumberIntType, &pid) && pid > 0) {
            CFRelease(windows);
            return (pid_t)pid;
        }
    }

    CFRelease(windows);
    return 0;
}

static bool lockdock_shim_copy_ax_bounds(AXUIElementRef element,
                                         CGRect *bounds_out) {
    CGPoint position = CGPointZero;
    CGSize size = CGSizeZero;
    CFTypeRef value = NULL;
    bool has_position = false;
    bool has_size = false;

    if (AXUIElementCopyAttributeValue(element, kAXPositionAttribute, &value) ==
            kAXErrorSuccess &&
        value != NULL) {
        has_position = AXValueGetValue((AXValueRef)value, kAXValueCGPointType,
                                       &position);
        CFRelease(value);
    }

    value = NULL;
    if (AXUIElementCopyAttributeValue(element, kAXSizeAttribute, &value) ==
            kAXErrorSuccess &&
        value != NULL) {
        has_size = AXValueGetValue((AXValueRef)value, kAXValueCGSizeType, &size);
        CFRelease(value);
    }

    if (!has_position) {
        return false;
    }

    *bounds_out = CGRectMake(position.x, position.y, has_size ? size.width : 1.0,
                             has_size ? size.height : 1.0);
    return true;
}

bool lockdock_shim_copy_accessibility_dock_window_bounds(LockDockShimRect *rect_out) {
    pid_t dock_pid = lockdock_shim_find_dock_pid();
    AXUIElementRef app;
    CFArrayRef windows = NULL;
    CGRect best_bounds = CGRectZero;
    double best_area = 0;

    if (rect_out == NULL || dock_pid <= 0 || !AXIsProcessTrusted()) {
        return false;
    }

    app = AXUIElementCreateApplication(dock_pid);
    if (app == NULL) {
        return false;
    }

    if (AXUIElementCopyAttributeValue(app, kAXWindowsAttribute,
                                      (CFTypeRef *)&windows) != kAXErrorSuccess ||
        windows == NULL) {
        CFRelease(app);
        return false;
    }

    for (CFIndex i = 0; i < CFArrayGetCount(windows); i++) {
        AXUIElementRef window = (AXUIElementRef)CFArrayGetValueAtIndex(windows, i);
        CGRect bounds = CGRectZero;
        double area;

        if (!lockdock_shim_copy_ax_bounds(window, &bounds)) {
            continue;
        }

        area = bounds.size.width * bounds.size.height;
        if (area > best_area) {
            best_area = area;
            best_bounds = bounds;
        }
    }

    CFRelease(windows);
    CFRelease(app);

    if (CGRectIsNull(best_bounds) || CGRectIsEmpty(best_bounds)) {
        return false;
    }

    *rect_out = lockdock_shim_rect_from_cg(best_bounds);
    return true;
}

bool lockdock_shim_copy_dock_orientation(char *buffer, size_t buffer_size) {
    CFPropertyListRef value;
    bool copied = false;

    if (buffer == NULL || buffer_size == 0) {
        return false;
    }

    buffer[0] = '\0';
    value = CFPreferencesCopyAppValue(CFSTR("orientation"), CFSTR("com.apple.dock"));
    if (value == NULL) {
        return false;
    }

    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        copied = CFStringGetCString((CFStringRef)value, buffer, buffer_size,
                                    kCFStringEncodingUTF8);
    }

    CFRelease(value);
    return copied;
}

bool lockdock_shim_copy_mouse_location(LockDockShimPoint *point_out) {
    CGEventRef event;
    CGPoint point;

    if (point_out == NULL) {
        return false;
    }

    event = CGEventCreate(NULL);
    if (event == NULL) {
        return false;
    }

    point = CGEventGetLocation(event);
    CFRelease(event);
    point_out->x = point.x;
    point_out->y = point.y;
    return true;
}

void *lockdock_shim_event_source_create(void) {
    return CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
}

void lockdock_shim_event_source_set_suppression_interval(void *source,
                                                         double interval) {
    if (source != NULL) {
        CGEventSourceSetLocalEventsSuppressionInterval((CGEventSourceRef)source,
                                                       interval);
    }
}

void lockdock_shim_release(void *object) {
    if (object != NULL) {
        CFRelease(object);
    }
}

bool lockdock_shim_set_cursor_association(bool associated) {
    return CGAssociateMouseAndMouseCursorPosition(associated) == kCGErrorSuccess;
}

void lockdock_shim_warp_mouse(LockDockShimPoint point) {
    CGWarpMouseCursorPosition(CGPointMake(point.x, point.y));
}

void lockdock_shim_post_mouse_moved(void *source, LockDockShimPoint point) {
    CGEventRef event = CGEventCreateMouseEvent((CGEventSourceRef)source,
                                               kCGEventMouseMoved,
                                               CGPointMake(point.x, point.y),
                                               kCGMouseButtonLeft);

    if (event != NULL) {
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }
}

void lockdock_shim_post_edge_nudge(void *source,
                                   LockDockShimPoint point,
                                   int orientation) {
    CGEventRef event = CGEventCreateMouseEvent((CGEventSourceRef)source,
                                               kCGEventMouseMoved,
                                               CGPointMake(point.x, point.y),
                                               kCGMouseButtonLeft);

    if (event == NULL) {
        return;
    }

    if (orientation == 0) {
        CGEventSetIntegerValueField(event, kCGMouseEventDeltaY, 1);
    } else if (orientation == 1) {
        CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, -1);
    } else if (orientation == 2) {
        CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, 1);
    }

    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

static void lockdock_shim_event_tap_enable(void) {
    if (g_event_tap != NULL) {
        CGEventTapEnable(g_event_tap, true);
    }
}

static CGEventRef lockdock_shim_event_callback(CGEventTapProxy proxy,
                                               CGEventType type,
                                               CGEventRef event,
                                               void *user_info) {
    CGPoint point;
    int kind = LOCKDOCK_SHIM_EVENT_OTHER;

    (void)proxy;
    (void)user_info;

    if (type == kCGEventTapDisabledByTimeout ||
        type == kCGEventTapDisabledByUserInput) {
        lockdock_shim_event_tap_enable();
        return event;
    }

    if (type == kCGEventMouseMoved) {
        kind = LOCKDOCK_SHIM_EVENT_MOUSE_MOVED;
    } else if (type == kCGEventLeftMouseDragged ||
               type == kCGEventRightMouseDragged ||
               type == kCGEventOtherMouseDragged) {
        kind = LOCKDOCK_SHIM_EVENT_MOUSE_DRAGGED;
    } else {
        return event;
    }

    point = CGEventGetLocation(event);
    if (lockdock_display_should_suppress_event(kind, point.x, point.y)) {
        return NULL;
    }

    return event;
}

static void lockdock_shim_publish_started(CFRunLoopRef run_loop) {
    pthread_mutex_lock(&g_event_mutex);
    g_event_run_loop = run_loop;
    g_event_thread_running = true;
    g_event_thread_starting = false;
    g_event_error[0] = '\0';
    pthread_cond_broadcast(&g_event_cond);
    pthread_mutex_unlock(&g_event_mutex);
}

static void lockdock_shim_publish_failed(const char *message) {
    pthread_mutex_lock(&g_event_mutex);
    g_event_thread_running = false;
    g_event_thread_starting = false;
    snprintf(g_event_error, sizeof(g_event_error), "%s", message);
    pthread_cond_broadcast(&g_event_cond);
    pthread_mutex_unlock(&g_event_mutex);
}

static void lockdock_shim_publish_stopped(void) {
    pthread_mutex_lock(&g_event_mutex);
    g_event_tap = NULL;
    g_event_source = NULL;
    g_event_run_loop = NULL;
    g_event_thread_running = false;
    pthread_cond_broadcast(&g_event_cond);
    pthread_mutex_unlock(&g_event_mutex);
}

static void *lockdock_shim_event_thread(void *context) {
    CGEventMask mask;
    CFRunLoopRef run_loop;

    (void)context;

    mask = CGEventMaskBit(kCGEventMouseMoved) |
           CGEventMaskBit(kCGEventLeftMouseDragged) |
           CGEventMaskBit(kCGEventRightMouseDragged) |
           CGEventMaskBit(kCGEventOtherMouseDragged);

    g_event_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                   kCGEventTapOptionDefault, mask,
                                   lockdock_shim_event_callback, NULL);
    if (g_event_tap == NULL) {
        lockdock_shim_publish_failed(
            "Failed to create event tap. Grant Accessibility permission in System Settings");
        return NULL;
    }

    g_event_source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault,
                                                   g_event_tap, 0);
    if (g_event_source == NULL) {
        CFRelease(g_event_tap);
        g_event_tap = NULL;
        lockdock_shim_publish_failed("Failed to create event tap source");
        return NULL;
    }

    run_loop = CFRunLoopGetCurrent();
    CFRunLoopAddSource(run_loop, g_event_source, kCFRunLoopCommonModes);
    lockdock_shim_event_tap_enable();
    lockdock_shim_publish_started(run_loop);

    CFRunLoopRun();

    if (g_event_tap != NULL) {
        CGEventTapEnable(g_event_tap, false);
    }
    if (g_event_source != NULL) {
        CFRunLoopRemoveSource(run_loop, g_event_source, kCFRunLoopCommonModes);
        CFRelease(g_event_source);
    }
    if (g_event_tap != NULL) {
        CFRelease(g_event_tap);
    }

    lockdock_shim_publish_stopped();
    return NULL;
}

static void lockdock_shim_wait_for_start(void) {
    while (g_event_thread_starting) {
        pthread_cond_wait(&g_event_cond, &g_event_mutex);
    }
}

bool lockdock_shim_start_event_tap(char *error, size_t error_size) {
    pthread_t stale_thread;
    bool has_stale_thread = false;

    pthread_mutex_lock(&g_event_mutex);
    lockdock_shim_wait_for_start();

    if (g_event_thread_running) {
        pthread_mutex_unlock(&g_event_mutex);
        return true;
    }

    if (g_event_thread_joinable) {
        stale_thread = g_event_thread;
        g_event_thread_joinable = false;
        has_stale_thread = true;
    }

    g_event_thread_starting = true;
    g_event_error[0] = '\0';
    pthread_mutex_unlock(&g_event_mutex);

    if (has_stale_thread) {
        pthread_join(stale_thread, NULL);
    }

    if (pthread_create(&g_event_thread, NULL, lockdock_shim_event_thread, NULL) !=
        0) {
        pthread_mutex_lock(&g_event_mutex);
        g_event_thread_starting = false;
        pthread_mutex_unlock(&g_event_mutex);
        snprintf(error, error_size, "Failed to start locker event thread");
        return false;
    }

    pthread_mutex_lock(&g_event_mutex);
    g_event_thread_joinable = true;
    lockdock_shim_wait_for_start();
    if (g_event_thread_running) {
        pthread_mutex_unlock(&g_event_mutex);
        return true;
    }

    snprintf(error, error_size, "%s", g_event_error);
    stale_thread = g_event_thread;
    g_event_thread_joinable = false;
    pthread_mutex_unlock(&g_event_mutex);
    pthread_join(stale_thread, NULL);
    return false;
}

void lockdock_shim_stop_event_tap(void) {
    pthread_t thread;
    CFRunLoopRef run_loop = NULL;
    bool join_thread = false;

    pthread_mutex_lock(&g_event_mutex);
    lockdock_shim_wait_for_start();

    if (g_event_thread_joinable) {
        thread = g_event_thread;
        run_loop = g_event_run_loop;
        g_event_thread_joinable = false;
        join_thread = true;
    }

    if (run_loop != NULL) {
        CFRunLoopStop(run_loop);
        CFRunLoopWakeUp(run_loop);
    }

    pthread_mutex_unlock(&g_event_mutex);

    if (join_thread) {
        pthread_join(thread, NULL);
    }
}
