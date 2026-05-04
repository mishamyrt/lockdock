#include "lockdockd_objc_bridge.h"

#include <ApplicationServices/ApplicationServices.h>
#include <AppKit/AppKit.h>
#include <Foundation/Foundation.h>
#include <IOKit/graphics/IOGraphicsLib.h>
#include <stdio.h>

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
            CFDictionaryRef info = IODisplayCreateInfoDictionary(
                service, kIODisplayOnlyPreferredName);
            IOObjectRelease(service);

            if (info != NULL) {
                NSDictionary *display_info = CFBridgingRelease(info);
                NSDictionary *names = display_info[[
                    NSString stringWithUTF8String:kDisplayProductName]];

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
                lockdockd_copy_nsstring(screen.localizedName, buffer,
                                        buffer_size)) {
                return true;
            }
        }
    }

    return false;
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
        NSRunningApplication *dock_app =
            [NSRunningApplication
                runningApplicationsWithBundleIdentifier:@"com.apple.dock"]
                .firstObject;
        AXUIElementRef dock_element;
        CFArrayRef windows = NULL;
        AXUIElementRef window;
        CGPoint position = CGPointZero;
        CFTypeRef position_value = NULL;
        AXError error;
        bool has_position = false;
        CGDirectDisplayID displays[32];
        uint32_t count = 0;

        if (dock_app == nil) {
            return 0;
        }

        dock_element =
            AXUIElementCreateApplication((pid_t)dock_app.processIdentifier);
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

        window = (AXUIElementRef)CFArrayGetValueAtIndex(windows, 0);
        error =
            AXUIElementCopyAttributeValue(window, kAXPositionAttribute,
                                          &position_value);
        if (error == kAXErrorSuccess && position_value != NULL) {
            has_position = AXValueGetValue((AXValueRef)position_value,
                                           kAXValueCGPointType, &position);
            CFRelease(position_value);
        }

        CFRelease(windows);
        CFRelease(dock_element);

        if (!has_position) {
            return 0;
        }

        CGGetActiveDisplayList(32, displays, &count);

        for (uint32_t i = 0; i < count; i++) {
            CGRect bounds = CGDisplayBounds(displays[i]);

            if (position.x >= bounds.origin.x &&
                position.x < bounds.origin.x + bounds.size.width &&
                position.y >= bounds.origin.y &&
                position.y < bounds.origin.y + bounds.size.height) {
                return displays[i];
            }
        }
    }

    return 0;
}
