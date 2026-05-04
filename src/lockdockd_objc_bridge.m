#include "lockdockd_objc_bridge.h"

#include <ApplicationServices/ApplicationServices.h>
#include <AppKit/AppKit.h>
#include <Foundation/Foundation.h>
#include <IOKit/graphics/IOGraphicsLib.h>
#include <limits.h>
#include <stdio.h>

#define LOCKDOCKD_MAX_DISPLAYS 32

static bool g_cursor_hide_used_core_graphics = false;
static bool g_cursor_hide_used_appkit = false;
static bool g_cursor_hide_activated_self = false;

static bool lockdockd_copy_nsstring(NSString *string,
                                    char *buffer,
                                    size_t buffer_size) {
    const char *utf8;

    if (string == nil || buffer == NULL || buffer_size == 0) {
        return false;
    }

    utf8 = string.UTF8String;
    if (utf8 == NULL || utf8[0] == '\0') {
        return false;
    }

    snprintf(buffer, buffer_size, "%s", utf8);
    return true;
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
            NSDictionary *dictionary = CFBridgingRelease(props);
            NSString *vendor_key = [NSString stringWithUTF8String:kDisplayVendorID];
            NSString *product_key =
                [NSString stringWithUTF8String:kDisplayProductID];
            NSString *serial_key =
                [NSString stringWithUTF8String:kDisplaySerialNumber];
            NSNumber *vendor_number = dictionary[vendor_key];
            NSNumber *product_number = dictionary[product_key];
            NSNumber *serial_number = dictionary[serial_key];

            if (vendor_number != nil && product_number != nil &&
                vendor_number.unsignedIntValue == vendor &&
                product_number.unsignedIntValue == product) {
                if (serial_number != nil &&
                    serial_number.unsignedIntValue == serial) {
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
    if (buffer == NULL || buffer_size == 0) {
        return false;
    }

    buffer[0] = '\0';

    @autoreleasepool {
        io_service_t service = lockdockd_find_display_service(display_id);

        if (service != MACH_PORT_NULL) {
            CFDictionaryRef info =
                IODisplayCreateInfoDictionary(service, kIODisplayOnlyPreferredName);
            IOObjectRelease(service);

            if (info != NULL) {
                NSDictionary *display_info = CFBridgingRelease(info);
                NSDictionary *names = display_info[
                    [NSString stringWithUTF8String:kDisplayProductName]];

                if ([names isKindOfClass:[NSDictionary class]]) {
                    NSString *name = names.allValues.firstObject;

                    if ([name isKindOfClass:[NSString class]] &&
                        lockdockd_copy_nsstring(name, buffer, buffer_size)) {
                        return true;
                    }
                }
            }
        }

        for (NSScreen *screen in NSScreen.screens) {
            NSNumber *screen_id = screen.deviceDescription[@"NSScreenNumber"];

            if (screen_id != nil && screen_id.unsignedIntValue == display_id &&
                lockdockd_copy_nsstring(screen.localizedName, buffer, buffer_size)) {
                return true;
            }
        }
    }

    return false;
}

bool lockdockd_is_accessibility_trusted(void) {
    return AXIsProcessTrusted();
}

static NSRunningApplication *lockdockd_find_dock_app(void) {
    NSArray<NSString *> *bundle_identifiers =
        @[ @"com.apple.dock", @"com.apple.Dock.agent" ];

    for (NSString *bundle_identifier in bundle_identifiers) {
        NSRunningApplication *app =
            [NSRunningApplication
                runningApplicationsWithBundleIdentifier:bundle_identifier]
                .firstObject;

        if (app != nil) {
            return app;
        }
    }

    for (NSRunningApplication *app in NSWorkspace.sharedWorkspace
             .runningApplications) {
        NSString *name = app.localizedName ?: @"";
        NSString *bundle_identifier = app.bundleIdentifier ?: @"";
        NSString *path = app.executableURL.path ?: @"";

        if ([name isEqualToString:@"Dock"] ||
            [bundle_identifier isEqualToString:@"com.apple.dock"] ||
            [bundle_identifier isEqualToString:@"com.apple.Dock.agent"] ||
            [path hasSuffix:@"/Dock.app/Contents/MacOS/Dock"]) {
            return app;
        }
    }

    return nil;
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

static bool lockdockd_copy_dock_window_bounds(CGRect *bounds_out) {
    const CGWindowListOption options[] = {
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGWindowListOptionAll,
    };

    if (bounds_out == NULL) {
        return false;
    }

    for (size_t option_index = 0;
         option_index < (sizeof(options) / sizeof(options[0])); option_index++) {
        CFArrayRef windows_ref =
            CGWindowListCopyWindowInfo(options[option_index], kCGNullWindowID);
        NSArray *windows;
        CGRect best_bounds = CGRectZero;
        CGFloat best_area = 0;
        int best_layer = INT_MIN;

        if (windows_ref == NULL) {
            continue;
        }

        windows = CFBridgingRelease(windows_ref);

        for (NSDictionary *window in windows) {
            NSString *owner = window[(NSString *)kCGWindowOwnerName];
            NSDictionary *window_bounds = window[(NSString *)kCGWindowBounds];
            NSString *name = window[(NSString *)kCGWindowName];
            NSNumber *layer_number = window[(NSString *)kCGWindowLayer];
            CGRect bounds = CGRectZero;
            CGFloat area;
            int layer = INT_MIN;

            if (![owner isKindOfClass:[NSString class]] ||
                ![owner isEqualToString:@"Dock"]) {
                continue;
            }

            if ([name isKindOfClass:[NSString class]] &&
                [name hasPrefix:@"Wallpaper-"]) {
                continue;
            }

            if (![window_bounds isKindOfClass:[NSDictionary class]] ||
                !CGRectMakeWithDictionaryRepresentation(
                    (CFDictionaryRef)window_bounds, &bounds) ||
                CGRectIsEmpty(bounds) || CGRectIsNull(bounds)) {
                continue;
            }

            if ([layer_number isKindOfClass:[NSNumber class]]) {
                layer = layer_number.intValue;
            }

            area = bounds.size.width * bounds.size.height;
            if (layer > best_layer || (layer == best_layer && area > best_area)) {
                best_layer = layer;
                best_area = area;
                best_bounds = bounds;
            }
        }

        if (!CGRectIsEmpty(best_bounds) && !CGRectIsNull(best_bounds)) {
            *bounds_out = best_bounds;
            return true;
        }
    }

    return false;
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

    error =
        AXUIElementCopyAttributeValue(element, kAXPositionAttribute, &position_value);
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
    NSRunningApplication *dock_app = lockdockd_find_dock_app();
    AXUIElementRef dock_element;
    CFArrayRef windows = NULL;
    CGRect best_bounds = CGRectZero;
    CGFloat best_area = 0;
    AXError error;

    if (!lockdockd_is_accessibility_trusted() || dock_app == nil) {
        return 0;
    }

    dock_element = AXUIElementCreateApplication((pid_t)dock_app.processIdentifier);
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
    @autoreleasepool {
        NSUserDefaults *dock =
            [[NSUserDefaults alloc] initWithSuiteName:@"com.apple.dock"];
        NSString *orientation = [dock stringForKey:@"orientation"];

        if ([orientation isEqualToString:@"left"]) {
            return LOCKDOCKD_ORIENT_LEFT;
        }

        if ([orientation isEqualToString:@"right"]) {
            return LOCKDOCKD_ORIENT_RIGHT;
        }
    }

    return LOCKDOCKD_ORIENT_BOTTOM;
}

CGDirectDisplayID lockdockd_get_dock_display(void) {
    @autoreleasepool {
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

    return 0;
}
