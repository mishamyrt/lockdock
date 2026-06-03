#include "display.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <limits.h>

// Maybe 16 is enough, but we'll be safe.
#define LOCKDOCK_DISPLAY_MAX_WINDOWS 256

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

static bool lockdock_display_copy_dock_window_bounds_for_option(
    CGWindowListOption option,
    LockDockDisplayRect *rect_out) {
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
            !lockdock_display_owner_is_dock(window) ||
            lockdock_display_window_is_wallpaper(window) ||
            !lockdock_display_copy_window_bounds(window, &bounds)) {
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

    *rect_out = lockdock_display_rect_from_cg(best_bounds);
    return true;
}

bool lockdock_display_copy_dock_window_bounds(LockDockDisplayRect *rect_out) {
    if (rect_out == NULL) {
        return false;
    }

    return lockdock_display_copy_dock_window_bounds_for_option(
               kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
               rect_out) ||
           lockdock_display_copy_dock_window_bounds_for_option(kCGWindowListOptionAll,
                                                               rect_out);
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

bool lockdock_display_copy_dock_orientation(char *buffer, size_t buffer_size) {
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
