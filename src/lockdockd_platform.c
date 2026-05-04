#include "lockdockd_platform.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/graphics/IOGraphicsLib.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#define LOCKDOCKD_MAX_DISPLAYS 32

static bool lockdockd_copy_cfstring(CFStringRef string,
                                    char *buffer,
                                    size_t buffer_size) {
    if (string == NULL || buffer == NULL || buffer_size == 0) {
        return false;
    }

    if (CFGetTypeID(string) != CFStringGetTypeID()) {
        return false;
    }

    if (!CFStringGetCString(string, buffer, (CFIndex)buffer_size,
                            kCFStringEncodingUTF8) ||
        buffer[0] == '\0') {
        return false;
    }

    return true;
}

static bool lockdockd_cfnumber_equals_uint32(CFTypeRef value, uint32_t expected) {
    uint32_t actual = 0;

    if (value == NULL || CFGetTypeID(value) != CFNumberGetTypeID()) {
        return false;
    }

    return CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &actual) &&
           actual == expected;
}

static bool lockdockd_copy_preferred_display_name(CFDictionaryRef info,
                                                  char *buffer,
                                                  size_t buffer_size) {
    CFDictionaryRef names;
    CFIndex count;
    const void **keys;
    const void **values;
    bool copied = false;

    if (info == NULL || buffer == NULL || buffer_size == 0) {
        return false;
    }

    names = (CFDictionaryRef)CFDictionaryGetValue(info, CFSTR(kDisplayProductName));
    if (names == NULL || CFGetTypeID(names) != CFDictionaryGetTypeID()) {
        return false;
    }

    count = CFDictionaryGetCount(names);
    if (count <= 0) {
        return false;
    }

    keys = (const void **)calloc((size_t)count, sizeof(*keys));
    values = (const void **)calloc((size_t)count, sizeof(*values));
    if (keys == NULL || values == NULL) {
        free(keys);
        free(values);
        return false;
    }

    CFDictionaryGetKeysAndValues(names, keys, values);

    for (CFIndex i = 0; i < count; i++) {
        if (lockdockd_copy_cfstring((CFStringRef)values[i], buffer, buffer_size)) {
            copied = true;
            break;
        }
    }

    free(keys);
    free(values);
    return copied;
}

static io_service_t lockdockd_find_display_service(CGDirectDisplayID display_id) {
    uint32_t vendor = CGDisplayVendorNumber(display_id);
    uint32_t product = CGDisplayModelNumber(display_id);
    uint32_t serial = CGDisplaySerialNumber(display_id);
    io_iterator_t iter = IO_OBJECT_NULL;
    CFMutableDictionaryRef match;
    io_service_t best = MACH_PORT_NULL;
    io_service_t service;

    if (vendor == 0 && product == 0) {
        return MACH_PORT_NULL;
    }

    match = IOServiceMatching("IODisplayConnect");
    if (match == NULL) {
        return MACH_PORT_NULL;
    }

    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) !=
            KERN_SUCCESS ||
        iter == IO_OBJECT_NULL) {
        return MACH_PORT_NULL;
    }

    while ((service = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        CFMutableDictionaryRef props = NULL;

        if (IORegistryEntryCreateCFProperties(service, &props, kCFAllocatorDefault,
                                              0) == KERN_SUCCESS &&
            props != NULL) {
            bool vendor_matches = lockdockd_cfnumber_equals_uint32(
                CFDictionaryGetValue(props, CFSTR(kDisplayVendorID)), vendor);
            bool product_matches = lockdockd_cfnumber_equals_uint32(
                CFDictionaryGetValue(props, CFSTR(kDisplayProductID)), product);
            bool serial_matches = lockdockd_cfnumber_equals_uint32(
                CFDictionaryGetValue(props, CFSTR(kDisplaySerialNumber)), serial);

            CFRelease(props);

            if (vendor_matches && product_matches) {
                if (serial_matches) {
                    if (best != MACH_PORT_NULL) {
                        IOObjectRelease(best);
                    }
                    best = service;
                    break;
                }

                if (best == MACH_PORT_NULL) {
                    best = service;
                    continue;
                }
            }
        }

        IOObjectRelease(service);
    }

    IOObjectRelease(iter);
    return best;
}

bool lockdockd_copy_display_name(CGDirectDisplayID display_id,
                                 char *buffer,
                                 size_t buffer_size) {
    io_service_t service;
    CFDictionaryRef info;
    bool copied = false;

    if (buffer == NULL || buffer_size == 0) {
        return false;
    }

    buffer[0] = '\0';

    service = lockdockd_find_display_service(display_id);
    if (service == MACH_PORT_NULL) {
        return false;
    }

    info = IODisplayCreateInfoDictionary(service, kIODisplayOnlyPreferredName);
    IOObjectRelease(service);

    if (info == NULL) {
        return false;
    }

    copied = lockdockd_copy_preferred_display_name(info, buffer, buffer_size);
    CFRelease(info);
    return copied;
}

bool lockdockd_is_accessibility_trusted(void) {
    return AXIsProcessTrusted();
}

static CGDirectDisplayID lockdockd_display_for_rect(CGRect rect) {
    CGDirectDisplayID displays[LOCKDOCKD_MAX_DISPLAYS];
    uint32_t count = 0;
    CGDirectDisplayID best_display = 0;
    CGFloat best_area = 0;

    if (CGRectIsEmpty(rect) || CGRectIsNull(rect)) {
        return 0;
    }

    CGGetActiveDisplayList(LOCKDOCKD_MAX_DISPLAYS, displays, &count);

    for (uint32_t i = 0; i < count; i++) {
        CGRect bounds = CGDisplayBounds(displays[i]);
        CGRect intersection = CGRectIntersection(bounds, rect);

        if (CGRectIsNull(intersection) || CGRectIsEmpty(intersection)) {
            continue;
        }

        CGFloat area = intersection.size.width * intersection.size.height;
        if (area > best_area) {
            best_area = area;
            best_display = displays[i];
        }
    }

    if (best_display != 0) {
        return best_display;
    }

    return lockdockd_find_display_at_point(
        CGPointMake(CGRectGetMidX(rect), CGRectGetMidY(rect)));
}

static bool lockdockd_copy_dock_window_bounds_for_option(CGWindowListOption option,
                                                         CGRect *bounds_out) {
    CFArrayRef windows_ref = CGWindowListCopyWindowInfo(option, kCGNullWindowID);
    CGRect best_bounds = CGRectZero;
    CGFloat best_area = 0;
    int best_layer = INT_MIN;

    if (windows_ref == NULL) {
        return false;
    }

    for (CFIndex i = 0; i < CFArrayGetCount(windows_ref); i++) {
        CFDictionaryRef window =
            (CFDictionaryRef)CFArrayGetValueAtIndex(windows_ref, i);
        CFStringRef owner = NULL;
        CFDictionaryRef window_bounds = NULL;
        CFStringRef name = NULL;
        CFNumberRef layer_number = NULL;
        CGRect bounds = CGRectZero;
        CGFloat area;
        int layer = INT_MIN;

        if (window == NULL || CFGetTypeID(window) != CFDictionaryGetTypeID()) {
            continue;
        }

        owner = (CFStringRef)CFDictionaryGetValue(window, kCGWindowOwnerName);
        if (owner == NULL || CFGetTypeID(owner) != CFStringGetTypeID() ||
            CFStringCompare(owner, CFSTR("Dock"), 0) != kCFCompareEqualTo) {
            continue;
        }

        name = (CFStringRef)CFDictionaryGetValue(window, kCGWindowName);
        if (name != NULL && CFGetTypeID(name) == CFStringGetTypeID() &&
            CFStringHasPrefix(name, CFSTR("Wallpaper-"))) {
            continue;
        }

        window_bounds =
            (CFDictionaryRef)CFDictionaryGetValue(window, kCGWindowBounds);
        if (window_bounds == NULL ||
            CFGetTypeID(window_bounds) != CFDictionaryGetTypeID() ||
            !CGRectMakeWithDictionaryRepresentation(window_bounds, &bounds) ||
            CGRectIsEmpty(bounds) || CGRectIsNull(bounds)) {
            continue;
        }

        layer_number = (CFNumberRef)CFDictionaryGetValue(window, kCGWindowLayer);
        if (layer_number != NULL &&
            CFGetTypeID(layer_number) == CFNumberGetTypeID()) {
            CFNumberGetValue(layer_number, kCFNumberIntType, &layer);
        }

        area = bounds.size.width * bounds.size.height;
        if (layer > best_layer || (layer == best_layer && area > best_area)) {
            best_layer = layer;
            best_area = area;
            best_bounds = bounds;
        }
    }

    CFRelease(windows_ref);

    if (CGRectIsEmpty(best_bounds) || CGRectIsNull(best_bounds)) {
        return false;
    }

    *bounds_out = best_bounds;
    return true;
}

static bool lockdockd_copy_dock_window_bounds(CGRect *bounds_out) {
    const CGWindowListOption options[] = {
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGWindowListOptionAll,
    };

    if (bounds_out == NULL) {
        return false;
    }

    for (size_t i = 0; i < (sizeof(options) / sizeof(options[0])); i++) {
        if (lockdockd_copy_dock_window_bounds_for_option(options[i], bounds_out)) {
            return true;
        }
    }

    return false;
}

static pid_t lockdockd_find_dock_pid(void) {
    const CGWindowListOption options[] = {
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGWindowListOptionAll,
    };

    for (size_t option_index = 0;
         option_index < (sizeof(options) / sizeof(options[0])); option_index++) {
        CFArrayRef windows_ref =
            CGWindowListCopyWindowInfo(options[option_index], kCGNullWindowID);

        if (windows_ref == NULL) {
            continue;
        }

        for (CFIndex i = 0; i < CFArrayGetCount(windows_ref); i++) {
            CFDictionaryRef window =
                (CFDictionaryRef)CFArrayGetValueAtIndex(windows_ref, i);
            CFStringRef owner = NULL;
            CFNumberRef owner_pid = NULL;
            int pid_value = 0;

            if (window == NULL || CFGetTypeID(window) != CFDictionaryGetTypeID()) {
                continue;
            }

            owner = (CFStringRef)CFDictionaryGetValue(window, kCGWindowOwnerName);
            if (owner == NULL || CFGetTypeID(owner) != CFStringGetTypeID() ||
                CFStringCompare(owner, CFSTR("Dock"), 0) != kCFCompareEqualTo) {
                continue;
            }

            owner_pid = (CFNumberRef)CFDictionaryGetValue(window, kCGWindowOwnerPID);
            if (owner_pid == NULL || CFGetTypeID(owner_pid) != CFNumberGetTypeID() ||
                !CFNumberGetValue(owner_pid, kCFNumberIntType, &pid_value) ||
                pid_value <= 0) {
                continue;
            }

            CFRelease(windows_ref);
            return (pid_t)pid_value;
        }

        CFRelease(windows_ref);
    }

    return 0;
}

static bool lockdockd_copy_ax_element_bounds(AXUIElementRef element,
                                             CGRect *bounds_out) {
    CGPoint position = CGPointZero;
    CGSize size = CGSizeZero;
    CFTypeRef position_value = NULL;
    CFTypeRef size_value = NULL;
    AXError error;
    bool has_position = false;
    bool has_size = false;

    if (element == NULL || bounds_out == NULL) {
        return false;
    }

    error = AXUIElementCopyAttributeValue(element, kAXPositionAttribute,
                                          &position_value);
    if (error == kAXErrorSuccess && position_value != NULL) {
        has_position = AXValueGetValue((AXValueRef)position_value,
                                       kAXValueCGPointType, &position);
        CFRelease(position_value);
    }

    error = AXUIElementCopyAttributeValue(element, kAXSizeAttribute, &size_value);
    if (error == kAXErrorSuccess && size_value != NULL) {
        has_size =
            AXValueGetValue((AXValueRef)size_value, kAXValueCGSizeType, &size);
        CFRelease(size_value);
    }

    if (!has_position) {
        return false;
    }

    *bounds_out = CGRectMake(position.x, position.y, has_size ? size.width : 1.0,
                             has_size ? size.height : 1.0);
    return true;
}

static CGDirectDisplayID lockdockd_get_dock_display_via_accessibility(void) {
    pid_t dock_pid = lockdockd_find_dock_pid();
    AXUIElementRef dock_element;
    CFArrayRef windows = NULL;
    CGRect best_bounds = CGRectZero;
    CGFloat best_area = 0;
    AXError error;

    if (!lockdockd_is_accessibility_trusted() || dock_pid <= 0) {
        return 0;
    }

    dock_element = AXUIElementCreateApplication(dock_pid);
    if (dock_element == NULL) {
        return 0;
    }

    error = AXUIElementCopyAttributeValue(dock_element, kAXWindowsAttribute,
                                          (CFTypeRef *)&windows);
    if (error != kAXErrorSuccess || windows == NULL) {
        CFRelease(dock_element);
        return 0;
    }

    if (CFArrayGetCount(windows) == 0) {
        CFRelease(windows);
        CFRelease(dock_element);
        return 0;
    }

    for (CFIndex i = 0; i < CFArrayGetCount(windows); i++) {
        AXUIElementRef window = (AXUIElementRef)CFArrayGetValueAtIndex(windows, i);
        CGRect bounds = CGRectZero;
        CGFloat area;

        if (!lockdockd_copy_ax_element_bounds(window, &bounds)) {
            continue;
        }

        area = bounds.size.width * bounds.size.height;
        if (area > best_area) {
            best_area = area;
            best_bounds = bounds;
        }
    }

    CFRelease(windows);
    CFRelease(dock_element);

    if (!CGRectIsEmpty(best_bounds) && !CGRectIsNull(best_bounds)) {
        return lockdockd_display_for_rect(best_bounds);
    }

    return 0;
}

LockDockdDockOrientation lockdockd_get_dock_orientation(void) {
    CFPropertyListRef value =
        CFPreferencesCopyAppValue(CFSTR("orientation"), CFSTR("com.apple.dock"));

    if (value != NULL) {
        if (CFGetTypeID(value) == CFStringGetTypeID()) {
            CFStringRef orientation = (CFStringRef)value;

            if (CFStringCompare(orientation, CFSTR("left"), 0) ==
                kCFCompareEqualTo) {
                CFRelease(value);
                return LOCKDOCKD_ORIENT_LEFT;
            }

            if (CFStringCompare(orientation, CFSTR("right"), 0) ==
                kCFCompareEqualTo) {
                CFRelease(value);
                return LOCKDOCKD_ORIENT_RIGHT;
            }
        }

        CFRelease(value);
    }

    return LOCKDOCKD_ORIENT_BOTTOM;
}

CGDirectDisplayID lockdockd_get_dock_display(void) {
    CGRect dock_bounds = CGRectZero;
    CGDirectDisplayID dock_display = 0;

    if (lockdockd_copy_dock_window_bounds(&dock_bounds)) {
        dock_display = lockdockd_display_for_rect(dock_bounds);
        if (dock_display != 0) {
            return dock_display;
        }
    }

    return lockdockd_get_dock_display_via_accessibility();
}
