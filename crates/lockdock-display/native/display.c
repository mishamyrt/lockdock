#include "display.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <limits.h>
#include <stdatomic.h>

// Maybe 16 is enough, but we'll be safe.
#define LOCKDOCK_DISPLAY_MAX_WINDOWS 256
// A window covering this fraction of a display counts as display-covering.
#define LOCKDOCK_DISPLAY_COVERING_RATIO 0.9

// Window dictionaries are expensive to enumerate, so steady-state checks use
// the last validated Dock bar ID and fall back to discovery when it disappears.
static _Atomic(CGWindowID) g_dock_window_id = kCGNullWindowID;

static LockDockDisplayRect lockdock_display_rect_from_cg(CGRect rect) {
  LockDockDisplayRect out = {
    rect.origin.x,
    rect.origin.y,
    rect.size.width,
    rect.size.height
  };
  return out;
}

uint32_t lockdock_display_get_active_displays(uint32_t *displays, uint32_t max_displays) {
    uint32_t count = 0;

    if (displays == NULL || max_displays == 0) {
        return 0;
    }

    CGGetActiveDisplayList(max_displays, displays, &count);
    return count;
}

bool lockdock_display_copy_bounds(
    uint32_t display_id,
    LockDockDisplayRect *rect_out
) {
    CGRect bounds;

    if (display_id == 0 || rect_out == NULL) {
        return false;
    }

    bounds = CGDisplayBounds(display_id);
    if (CGRectIsNull(bounds) || CGRectIsEmpty(bounds)) {
        return false;
    }

    *rect_out = lockdock_display_rect_from_cg(bounds);
    return true;
}

bool lockdock_display_is_accessibility_trusted(void) {
    return AXIsProcessTrusted();
}

static bool lockdock_display_owner_is_dock(CFDictionaryRef window) {
    CFStringRef owner = (CFStringRef)CFDictionaryGetValue(window, kCGWindowOwnerName);

    return owner != NULL && CFGetTypeID(owner) == CFStringGetTypeID() &&
           CFStringCompare(owner, CFSTR("Dock"), 0) == kCFCompareEqualTo;
}

static bool lockdock_display_window_is_wallpaper(CFDictionaryRef window) {
    CFStringRef name = (CFStringRef)CFDictionaryGetValue(window, kCGWindowName);

    return name != NULL && CFGetTypeID(name) == CFStringGetTypeID() &&
           CFStringHasPrefix(name, CFSTR("Wallpaper-"));
}

static bool lockdock_display_copy_window_bounds(CFDictionaryRef window,
                                                CGRect *bounds_out) {
    CFDictionaryRef bounds =
        (CFDictionaryRef)CFDictionaryGetValue(window, kCGWindowBounds);

    return bounds != NULL && CFGetTypeID(bounds) == CFDictionaryGetTypeID() &&
           CGRectMakeWithDictionaryRepresentation(bounds, bounds_out) &&
           !CGRectIsNull(*bounds_out) && !CGRectIsEmpty(*bounds_out);
}

static int lockdock_display_copy_window_layer(CFDictionaryRef window) {
    CFNumberRef layer_number =
        (CFNumberRef)CFDictionaryGetValue(window, kCGWindowLayer);
    int layer = INT_MIN;

    if (layer_number != NULL && CFGetTypeID(layer_number) == CFNumberGetTypeID()) {
        CFNumberGetValue(layer_number, kCFNumberIntType, &layer);
    }

    return layer;
}

static CGWindowID lockdock_display_copy_window_id(CFDictionaryRef window) {
    CFNumberRef window_number =
        (CFNumberRef)CFDictionaryGetValue(window, kCGWindowNumber);
    int32_t window_id = 0;

    if (window_number != NULL && CFGetTypeID(window_number) == CFNumberGetTypeID() &&
        CFNumberGetValue(window_number, kCFNumberSInt32Type, &window_id)) {
        return (CGWindowID)window_id;
    }

    return kCGNullWindowID;
}

static double lockdock_display_intersection_area(CGRect left, CGRect right) {
    CGRect intersection = CGRectIntersection(left, right);

    if (CGRectIsNull(intersection) || CGRectIsEmpty(intersection)) {
        return 0.0;
    }

    return intersection.size.width * intersection.size.height;
}

static int lockdock_display_dock_window_level(void) {
    return (int)CGWindowLevelForKey(kCGDockWindowLevelKey);
}

static bool lockdock_display_copy_dock_bar_window(CFDictionaryRef window,
                                                  int dock_level,
                                                  CGRect *bounds_out,
                                                  CGWindowID *window_id_out) {
    CGWindowID window_id;

    if (window == NULL || CFGetTypeID(window) != CFDictionaryGetTypeID() ||
        !lockdock_display_owner_is_dock(window) ||
        lockdock_display_window_is_wallpaper(window) ||
        !lockdock_display_copy_window_bounds(window, bounds_out) ||
        lockdock_display_copy_window_layer(window) != dock_level) {
        return false;
    }

    window_id = lockdock_display_copy_window_id(window);
    if (window_id == kCGNullWindowID) {
        return false;
    }

    *window_id_out = window_id;
    return true;
}

static CFArrayRef lockdock_display_copy_window_info(CGWindowID window_id) {
    int32_t raw_window_id = (int32_t)window_id;
    CFNumberRef window_number =
        CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &raw_window_id);
    CFArrayRef window_ids;
    CFArrayRef windows;
    const void *values[1];

    if (window_number == NULL) {
        return NULL;
    }

    values[0] = window_number;
    window_ids =
        CFArrayCreate(kCFAllocatorDefault, values, 1, &kCFTypeArrayCallBacks);
    CFRelease(window_number);
    if (window_ids == NULL) {
        return NULL;
    }

    windows = CGWindowListCreateDescriptionFromArray(window_ids);
    CFRelease(window_ids);
    return windows;
}

static bool lockdock_display_copy_cached_dock_window_bounds(
    LockDockDisplayRect *rect_out) {
    CGWindowID cached_window_id =
        atomic_load_explicit(&g_dock_window_id, memory_order_relaxed);
    CFArrayRef windows;
    CGRect bounds = CGRectZero;
    CGWindowID window_id = kCGNullWindowID;
    int dock_level = lockdock_display_dock_window_level();
    bool found = false;

    if (cached_window_id == kCGNullWindowID) {
        return false;
    }

    windows = lockdock_display_copy_window_info(cached_window_id);
    if (windows != NULL) {
        if (CFArrayGetCount(windows) > 0) {
            CFDictionaryRef window =
                (CFDictionaryRef)CFArrayGetValueAtIndex(windows, 0);

            if (lockdock_display_copy_dock_bar_window(window, dock_level, &bounds,
                                                      &window_id) &&
                window_id == cached_window_id) {
                found = true;
            }
        }
        CFRelease(windows);
    }

    if (!found) {
        atomic_store_explicit(&g_dock_window_id, kCGNullWindowID,
                              memory_order_relaxed);
        return false;
    }

    *rect_out = lockdock_display_rect_from_cg(bounds);
    return true;
}

// The Dock bar window is the Dock-owned window sitting exactly at
// kCGDockWindowLevel. Everything else the Dock process may own (Mission
// Control and Expose backdrops on older macOS, wallpaper, hot corners) lives
// on other levels. On modern macOS the bar window spans its whole display, on
// older versions it is a thin strip; either way the level identifies it.
static bool lockdock_display_copy_dock_window_bounds_for_option(
    CGWindowListOption option,
    LockDockDisplayRect *rect_out) {
    CFArrayRef windows = CGWindowListCopyWindowInfo(option, kCGNullWindowID);
    CGRect best_bounds = CGRectZero;
    double best_area = 0;
    int dock_level = lockdock_display_dock_window_level();
    CGWindowID best_window_id = kCGNullWindowID;

    if (windows == NULL) {
        return false;
    }

    for (CFIndex i = 0; i < CFArrayGetCount(windows); i++) {
        CFDictionaryRef window = (CFDictionaryRef)CFArrayGetValueAtIndex(windows, i);
        CGRect bounds = CGRectZero;
        CGWindowID window_id = kCGNullWindowID;
        double area;

        if (!lockdock_display_copy_dock_bar_window(window, dock_level, &bounds,
                                                   &window_id)) {
            continue;
        }

        area = bounds.size.width * bounds.size.height;
        if (area > best_area) {
            best_area = area;
            best_bounds = bounds;
            best_window_id = window_id;
        }
    }

    CFRelease(windows);

    if (best_window_id == kCGNullWindowID || CGRectIsNull(best_bounds) ||
        CGRectIsEmpty(best_bounds)) {
        return false;
    }

    atomic_store_explicit(&g_dock_window_id, best_window_id, memory_order_relaxed);
    *rect_out = lockdock_display_rect_from_cg(best_bounds);
    return true;
}

bool lockdock_display_copy_dock_window_bounds(LockDockDisplayRect *rect_out) {
    if (rect_out == NULL) {
        return false;
    }

    return lockdock_display_copy_cached_dock_window_bounds(rect_out) ||
           lockdock_display_copy_dock_window_bounds_for_option(
               kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
               rect_out) ||
           lockdock_display_copy_dock_window_bounds_for_option(
               kCGWindowListOptionAll, rect_out);
}

static pid_t lockdock_display_find_dock_pid(void) {
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
            !lockdock_display_owner_is_dock(window)) {
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

static bool lockdock_display_copy_ax_bounds(AXUIElementRef element,
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

bool lockdock_display_copy_accessibility_dock_window_bounds(
    LockDockDisplayRect *rect_out) {
    pid_t dock_pid = lockdock_display_find_dock_pid();
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

        if (!lockdock_display_copy_ax_bounds(window, &bounds)) {
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

    *rect_out = lockdock_display_rect_from_cg(best_bounds);
    return true;
}

// While Mission Control or Expose is up, a shield window covers each display
// at a level above normal windows but below the Dock (WindowManager's
// "ExposeShieldWindow" on modern macOS, the Dock's own backdrops on older
// versions). Pushing the cursor at a display edge cannot summon the Dock
// until it goes away.
bool lockdock_display_dock_overlay_active(uint32_t display_id) {
    CGRect display;
    double display_area;
    CFArrayRef windows;
    int dock_level = lockdock_display_dock_window_level();
    bool active = false;

    if (display_id == 0) {
        return false;
    }

    display = CGDisplayBounds(display_id);
    if (CGRectIsNull(display) || CGRectIsEmpty(display)) {
        return false;
    }
    display_area = display.size.width * display.size.height;

    windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    if (windows == NULL) {
        return false;
    }

    for (CFIndex i = 0; i < CFArrayGetCount(windows); i++) {
        CFDictionaryRef window =
            (CFDictionaryRef)CFArrayGetValueAtIndex(windows, i);
        CGRect bounds = CGRectZero;
        int layer;

        if (window == NULL || CFGetTypeID(window) != CFDictionaryGetTypeID() ||
            !lockdock_display_copy_window_bounds(window, &bounds)) {
            continue;
        }

        layer = lockdock_display_copy_window_layer(window);
        if (layer <= 0 || layer >= dock_level) {
            continue;
        }

        if (lockdock_display_intersection_area(bounds, display) >=
            LOCKDOCK_DISPLAY_COVERING_RATIO * display_area) {
            active = true;
            break;
        }
    }

    CFRelease(windows);
    return active;
}
