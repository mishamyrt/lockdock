#include "lockdockd_platform.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#define JSON_TOKENIZER_IMPLEMENTATION
#include "../thirdparty/json_tokenizer.h"
#pragma clang diagnostic pop

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define LOCKDOCKD_MAX_DISPLAYS 32

#define LOCKDOCKD_SYSTEM_PROFILER_CACHE_TTL_SECONDS 5

typedef struct {
    CGDirectDisplayID display_id;
    char name[256];
} LockDockdDisplayNameEntry;

typedef struct {
    bool has_display_id;
    bool has_name;
    bool prefers_display_id;
    CGDirectDisplayID display_id;
    char name[256];
} LockDockdDisplayObjectState;

static LockDockdDisplayNameEntry
    lockdockd_display_name_cache[LOCKDOCKD_MAX_DISPLAYS];
static size_t lockdockd_display_name_cache_count = 0;
static time_t lockdockd_display_name_cache_loaded_at = 0;

static bool lockdockd_parse_display_id(const char *value,
                                       CGDirectDisplayID *display_id_out) {
    char *endptr = NULL;
    unsigned long parsed;

    if (value == NULL || display_id_out == NULL || value[0] == '\0') {
        return false;
    }

    parsed = strtoul(value, &endptr, 10);
    if (endptr == value || *endptr != '\0' || parsed == 0 || parsed > UINT32_MAX) {
        return false;
    }

    *display_id_out = (CGDirectDisplayID)parsed;
    return true;
}

static void lockdockd_cache_display_name(LockDockdDisplayNameEntry *entries,
                                         size_t *entry_count,
                                         CGDirectDisplayID display_id,
                                         const char *name) {
    if (entries == NULL || entry_count == NULL || name == NULL || name[0] == '\0' ||
        display_id == 0) {
        return;
    }

    for (size_t i = 0; i < *entry_count; i++) {
        if (entries[i].display_id == display_id) {
            snprintf(entries[i].name, sizeof(entries[i].name), "%s", name);
            return;
        }
    }

    if (*entry_count >= LOCKDOCKD_MAX_DISPLAYS) {
        return;
    }

    entries[*entry_count].display_id = display_id;
    snprintf(entries[*entry_count].name, sizeof(entries[*entry_count].name), "%s",
             name);
    (*entry_count)++;
}

static bool lockdockd_parse_system_profiler_file(const char *path,
                                                 LockDockdDisplayNameEntry *entries,
                                                 size_t *entry_count) {
    LockDockdDisplayObjectState object_stack[32];
    char current_name[128];
    json_t *json;
    json_token_t token;
    int object_depth = -1;

    if (path == NULL || entries == NULL || entry_count == NULL) {
        return false;
    }

    json = json_fopen(path);
    if (json == NULL) {
        return false;
    }

    memset(object_stack, 0, sizeof(object_stack));
    memset(current_name, 0, sizeof(current_name));
    *entry_count = 0;

    while ((token = json_next_token(json)) != JSON_END_DOCUMENT) {
        if (token == JSON_ERROR) {
            json_close(json);
            return false;
        }

        switch (token) {
            case JSON_START_OBJECT:
                if (object_depth + 1 >=
                    (int)(sizeof(object_stack) / sizeof(object_stack[0]))) {
                    json_close(json);
                    return false;
                }
                object_depth++;
                memset(&object_stack[object_depth], 0,
                       sizeof(object_stack[object_depth]));
                current_name[0] = '\0';
                break;

            case JSON_END_OBJECT:
                if (object_depth >= 0 && object_stack[object_depth].has_display_id &&
                    object_stack[object_depth].has_name) {
                    lockdockd_cache_display_name(
                        entries, entry_count, object_stack[object_depth].display_id,
                        object_stack[object_depth].name);
                }
                if (object_depth >= 0) {
                    object_depth--;
                }
                current_name[0] = '\0';
                break;

            case JSON_NAME:
                snprintf(current_name, sizeof(current_name), "%s",
                         json_get_name(json) == NULL ? "" : json_get_name(json));
                break;

            case JSON_STRING:
            case JSON_UINT64:
            case JSON_INT64:
                if (object_depth >= 0 && current_name[0] != '\0') {
                    const char *value = json_get_value(json);

                    if (value != NULL) {
                        if (strcmp(current_name, "_name") == 0) {
                            snprintf(object_stack[object_depth].name,
                                     sizeof(object_stack[object_depth].name), "%s",
                                     value);
                            object_stack[object_depth].has_name =
                                object_stack[object_depth].name[0] != '\0';
                        } else if (strcmp(current_name, "_spdisplays_displayID") ==
                                   0) {
                            object_stack[object_depth].has_display_id =
                                lockdockd_parse_display_id(
                                    value, &object_stack[object_depth].display_id);
                            object_stack[object_depth].prefers_display_id =
                                object_stack[object_depth].has_display_id;
                        } else if (!object_stack[object_depth].prefers_display_id &&
                                   strcmp(current_name, "_spdisplays_CGSDID") == 0) {
                            object_stack[object_depth].has_display_id =
                                lockdockd_parse_display_id(
                                    value, &object_stack[object_depth].display_id);
                        }
                    }
                }
                current_name[0] = '\0';
                break;

            default:
                current_name[0] = '\0';
                break;
        }
    }

    json_close(json);
    return *entry_count > 0;
}

static bool lockdockd_refresh_display_name_cache(void) {
    LockDockdDisplayNameEntry entries[LOCKDOCKD_MAX_DISPLAYS];
    size_t entry_count = 0;
    char path[] = "/tmp/lockdockd-system-profiler-XXXXXX";
    char command[PATH_MAX + 96];
    int fd;
    bool refreshed = false;

    memset(entries, 0, sizeof(entries));

    fd = mkstemp(path);
    if (fd < 0) {
        return false;
    }
    close(fd);

    if (snprintf(
            command, sizeof(command),
            "/usr/sbin/system_profiler -json SPDisplaysDataType > %s 2>/dev/null",
            path) >= (int)sizeof(command)) {
        unlink(path);
        return false;
    }

    if (system(command) == 0 &&
        lockdockd_parse_system_profiler_file(path, entries, &entry_count)) {
        memcpy(lockdockd_display_name_cache, entries, sizeof(entries));
        lockdockd_display_name_cache_count = entry_count;
        lockdockd_display_name_cache_loaded_at = time(NULL);
        refreshed = true;
    }

    unlink(path);
    return refreshed;
}

static bool lockdockd_copy_cached_display_name(CGDirectDisplayID display_id,
                                               char *buffer,
                                               size_t buffer_size) {
    if (buffer == NULL || buffer_size == 0) {
        return false;
    }

    for (size_t i = 0; i < lockdockd_display_name_cache_count; i++) {
        if (lockdockd_display_name_cache[i].display_id == display_id &&
            lockdockd_display_name_cache[i].name[0] != '\0') {
            snprintf(buffer, buffer_size, "%s",
                     lockdockd_display_name_cache[i].name);
            return true;
        }
    }

    return false;
}

bool lockdockd_copy_display_name(CGDirectDisplayID display_id,
                                 char *buffer,
                                 size_t buffer_size) {
    time_t now = time(NULL);

    if (buffer == NULL || buffer_size == 0) {
        return false;
    }

    buffer[0] = '\0';

    if (lockdockd_copy_cached_display_name(display_id, buffer, buffer_size)) {
        return true;
    }

    if (lockdockd_display_name_cache_loaded_at == 0 ||
        difftime(now, lockdockd_display_name_cache_loaded_at) >=
            LOCKDOCKD_SYSTEM_PROFILER_CACHE_TTL_SECONDS) {
        if (!lockdockd_refresh_display_name_cache()) {
            return false;
        }
    }

    if (lockdockd_copy_cached_display_name(display_id, buffer, buffer_size)) {
        return true;
    }

    if (!lockdockd_refresh_display_name_cache()) {
        return false;
    }

    return lockdockd_copy_cached_display_name(display_id, buffer, buffer_size);
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
