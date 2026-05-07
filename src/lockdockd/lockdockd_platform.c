#include "lockdockd_platform.h"

#include "lockdockd_display.h"

#include <json.h>

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LOCKDOCKD_MAX_DISPLAYS 32

#define LOCKDOCKD_SYSTEM_PROFILER_CACHE_TTL_SECONDS 5
#define LOCKDOCKD_DISPLAY_ID_MAX 64
#define LOCKDOCKD_DISPLAY_NAME_MAX 256
#define LOCKDOCKD_STREAM_BUF_INITIAL_CAP 4096

typedef struct {
    CGDirectDisplayID display_id;
    char name[LOCKDOCKD_DISPLAY_NAME_MAX];
} LockDockdDisplayNameEntry;

typedef struct {
    bool has_display_id;
    bool has_name;
    bool prefers_display_id;
    CGDirectDisplayID display_id;
    char name[LOCKDOCKD_DISPLAY_NAME_MAX];
} LockDockdDisplayObjectState;

static LockDockdDisplayNameEntry
    lockdockd_display_name_cache[LOCKDOCKD_MAX_DISPLAYS];
static size_t lockdockd_display_name_cache_count = 0;
static time_t lockdockd_display_name_cache_last_refresh_attempt_at = 0;
static bool lockdockd_display_name_cache_needs_refresh = true;
static _Atomic uint32_t g_dock_orientation_cache = 0;

static void lockdockd_clear_display_name_cache_entries(void) {
    memset(lockdockd_display_name_cache, 0, sizeof(lockdockd_display_name_cache));
    lockdockd_display_name_cache_count = 0;
}

void lockdockd_invalidate_display_name_cache(void) {
    lockdockd_clear_display_name_cache_entries();
    lockdockd_display_name_cache_last_refresh_attempt_at = 0;
    lockdockd_display_name_cache_needs_refresh = true;
}

static bool lockdockd_parse_display_id(const char *value,
                                       CGDirectDisplayID *display_id_out) {
    char *endptr = NULL;
    unsigned long parsed;

    if (value == NULL || display_id_out == NULL || value[0] == '\0') {
        return false;
    }

    parsed = strtoul(value, &endptr, 10);  // NOLINT
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

static bool lockdockd_json_name_equals(const json_string_t *name, const char *text) {
    size_t text_length;

    if (name == NULL || name->string == NULL || text == NULL) {
        return false;
    }

    text_length = strlen(text);
    return name->string_size == text_length &&
           memcmp(name->string, text, text_length) == 0;
}

static bool lockdockd_json_copy_string(const json_value_t *value,
                                       char *buffer,
                                       size_t buffer_size) {
    json_string_t *string_value;

    if (value == NULL || buffer == NULL || buffer_size == 0 ||
        value->type != json_type_string) {
        return false;
    }

    string_value = json_value_as_string((json_value_t *)value);
    if (string_value == NULL || string_value->string == NULL ||
        string_value->string_size >= buffer_size) {
        return false;
    }

    memcpy(buffer, string_value->string, string_value->string_size);
    buffer[string_value->string_size] = '\0';
    return true;
}

static bool lockdockd_json_copy_number_text(const json_value_t *value,
                                            char *buffer,
                                            size_t buffer_size) {
    json_number_t *number_value;

    if (value == NULL || buffer == NULL || buffer_size == 0 ||
        value->type != json_type_number) {
        return false;
    }

    number_value = json_value_as_number((json_value_t *)value);
    if (number_value == NULL || number_value->number == NULL ||
        number_value->number_size >= buffer_size) {
        return false;
    }

    memcpy(buffer, number_value->number, number_value->number_size);
    buffer[number_value->number_size] = '\0';
    return true;
}

static bool lockdockd_parse_display_id_json_value(
    const json_value_t *value,
    CGDirectDisplayID *display_id_out) {
    char text[LOCKDOCKD_DISPLAY_ID_MAX];

    if (value == NULL || display_id_out == NULL) {
        return false;
    }

    if (value->type == json_type_string) {
        return lockdockd_json_copy_string(value, text, sizeof(text)) &&
               lockdockd_parse_display_id(text, display_id_out);
    }

    if (value->type == json_type_number) {
        return lockdockd_json_copy_number_text(value, text, sizeof(text)) &&
               lockdockd_parse_display_id(text, display_id_out);
    }

    return false;
}

static void lockdockd_collect_display_names_from_value(
    const json_value_t *value,
    LockDockdDisplayNameEntry *entries,
    size_t *entry_count);

static void lockdockd_collect_display_names_from_object(
    const json_object_t *object,
    LockDockdDisplayNameEntry *entries,
    size_t *entry_count) {
    LockDockdDisplayObjectState object_state;
    json_object_element_t *element;

    if (object == NULL || entries == NULL || entry_count == NULL) {
        return;
    }

    memset(&object_state, 0, sizeof(object_state));

    for (element = object->start; element != NULL; element = element->next) {
        if (lockdockd_json_name_equals(element->name, "_name")) {
            if (lockdockd_json_copy_string(element->value, object_state.name,
                                           sizeof(object_state.name))) {
                object_state.has_name = object_state.name[0] != '\0';
            }
            continue;
        }

        if (lockdockd_json_name_equals(element->name, "_spdisplays_displayID")) {
            object_state.has_display_id = lockdockd_parse_display_id_json_value(
                element->value, &object_state.display_id);
            object_state.prefers_display_id = object_state.has_display_id;
            continue;
        }

        if (!object_state.prefers_display_id &&
            lockdockd_json_name_equals(element->name, "_spdisplays_CGSDID")) {
            object_state.has_display_id = lockdockd_parse_display_id_json_value(
                element->value, &object_state.display_id);
        }
    }

    if (object_state.has_display_id && object_state.has_name) {
        lockdockd_cache_display_name(entries, entry_count, object_state.display_id,
                                     object_state.name);
    }

    for (element = object->start; element != NULL; element = element->next) {
        lockdockd_collect_display_names_from_value(element->value, entries,
                                                   entry_count);
    }
}

static void lockdockd_collect_display_names_from_array(
    const json_array_t *array,
    LockDockdDisplayNameEntry *entries,
    size_t *entry_count) {
    json_array_element_t *element;

    if (array == NULL || entries == NULL || entry_count == NULL) {
        return;
    }

    for (element = array->start; element != NULL; element = element->next) {
        lockdockd_collect_display_names_from_value(element->value, entries,
                                                   entry_count);
    }
}

static void lockdockd_collect_display_names_from_value(
    const json_value_t *value,
    LockDockdDisplayNameEntry *entries,
    size_t *entry_count) {
    if (value == NULL || entries == NULL || entry_count == NULL) {
        return;
    }

    if (value->type == json_type_object) {
        lockdockd_collect_display_names_from_object(
            json_value_as_object((json_value_t *)value), entries, entry_count);
        return;
    }

    if (value->type == json_type_array) {
        lockdockd_collect_display_names_from_array(
            json_value_as_array((json_value_t *)value), entries, entry_count);
    }
}

static bool lockdockd_read_stream_to_buffer(FILE *stream,
                                            char **buffer_out,
                                            size_t *size_out) {
    char *buffer = NULL;
    size_t capacity = LOCKDOCKD_STREAM_BUF_INITIAL_CAP;
    size_t used = 0;

    if (stream == NULL || buffer_out == NULL || size_out == NULL) {
        return false;
    }

    buffer = (char *)malloc(capacity + 1);
    if (buffer == NULL) {
        return false;
    }

    while (true) {
        size_t remaining = capacity - used;
        size_t nread;

        if (remaining == 0) {
            size_t new_capacity = capacity * 2;
            char *new_buffer = (char *)realloc(buffer, new_capacity + 1);

            if (new_buffer == NULL) {
                free(buffer);
                return false;
            }

            buffer = new_buffer;
            capacity = new_capacity;
            remaining = capacity - used;
        }

        nread = fread(buffer + used, 1, remaining, stream);
        used += nread;

        if (nread < remaining) {
            if (ferror(stream)) {
                free(buffer);
                return false;
            }
            break;
        }
    }

    buffer[used] = '\0';
    *buffer_out = buffer;
    *size_out = used;
    return true;
}

static bool lockdockd_parse_system_profiler_stream(
    FILE *stream,
    LockDockdDisplayNameEntry *entries,
    size_t *entry_count) {
    char *json_buffer = NULL;
    size_t json_size = 0;
    json_parse_result_t parse_result;
    json_value_t *root = NULL;
    bool parsed = false;

    if (stream == NULL || entries == NULL || entry_count == NULL) {
        return false;
    }

    *entry_count = 0;
    memset(&parse_result, 0, sizeof(parse_result));

    if (!lockdockd_read_stream_to_buffer(stream, &json_buffer, &json_size)) {
        return false;
    }

    root = json_parse_ex(json_buffer, json_size, json_parse_flags_default, NULL,
                         NULL, &parse_result);
    if (root != NULL) {
        lockdockd_collect_display_names_from_value(root, entries, entry_count);
        parsed = *entry_count > 0;
    }

    free(root);
    free(json_buffer);
    return parsed;
}

static bool lockdockd_wait_for_process(pid_t pid, int *status_out) {
    int status = 0;

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return false;
        }
    }

    if (status_out != NULL) {
        *status_out = status;
    }

    return true;
}

static bool lockdockd_refresh_display_name_cache(void) {
    const char *const argv[] = {"/usr/sbin/system_profiler", "-json",
                                "SPDisplaysDataType", NULL};
    LockDockdDisplayNameEntry entries[LOCKDOCKD_MAX_DISPLAYS];
    size_t entry_count = 0;
    time_t refresh_attempt_at = time(NULL);
    int pipefd[2];
    pid_t pid;
    FILE *stream = NULL;
    int status = 0;
    bool parsed = false;
    bool refreshed = false;

    memset(entries, 0, sizeof(entries));
    lockdockd_display_name_cache_last_refresh_attempt_at = refresh_attempt_at;
    lockdockd_display_name_cache_needs_refresh = false;

    if (pipe(pipefd) != 0) {
        return false;
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        int stderr_fd = -1;

        close(pipefd[0]);

        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            _exit(127);  // NOLINT
        }

        stderr_fd = open("/dev/null", O_WRONLY);
        if (stderr_fd < 0 || dup2(stderr_fd, STDERR_FILENO) < 0) {
            if (stderr_fd >= 0) {
                close(stderr_fd);
            }
            _exit(127);  // NOLINT
        }

        close(stderr_fd);
        close(pipefd[1]);
        execv(argv[0], (char *const *)argv);
        _exit(127);  // NOLINT
    }

    close(pipefd[1]);
    stream = fdopen(pipefd[0], "r");
    if (stream != NULL) {
        parsed =
            lockdockd_parse_system_profiler_stream(stream, entries, &entry_count);
        fclose(stream);
    } else {
        close(pipefd[0]);
    }

    if (lockdockd_wait_for_process(pid, &status) && parsed && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0) {
        memcpy(lockdockd_display_name_cache, entries, sizeof(entries));
        lockdockd_display_name_cache_count = entry_count;
        refreshed = true;
    }

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

    if (lockdockd_display_name_cache_needs_refresh ||
        lockdockd_display_name_cache_last_refresh_attempt_at == 0 ||
        difftime(now, lockdockd_display_name_cache_last_refresh_attempt_at) >=
            LOCKDOCKD_SYSTEM_PROFILER_CACHE_TTL_SECONDS) {
        if (!lockdockd_refresh_display_name_cache()) {
            return false;
        }
    }

    if (lockdockd_copy_cached_display_name(display_id, buffer, buffer_size)) {
        return true;
    }

    return false;
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

static LockDockdDockOrientation lockdockd_copy_dock_orientation_value(void) {
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

void lockdockd_invalidate_dock_orientation_cache(void) {
    atomic_store(&g_dock_orientation_cache, 0);
}

LockDockdDockOrientation lockdockd_get_dock_orientation(void) {
    uint32_t cached = atomic_load(&g_dock_orientation_cache);

    if (cached != 0) {
        return (LockDockdDockOrientation)(cached - 1);
    }

    LockDockdDockOrientation orientation = lockdockd_copy_dock_orientation_value();

    atomic_store(&g_dock_orientation_cache, (uint32_t)orientation + 1);
    return orientation;
}

void lockdockd_reset_dock_probe(LockDockdDockProbe *probe) {
    if (probe == NULL) {
        return;
    }

    memset(probe, 0, sizeof(*probe));
}

bool lockdockd_capture_dock_probe(LockDockdDockProbe *probe) {
    if (probe == NULL) {
        return false;
    }

    lockdockd_reset_dock_probe(probe);

    if (!lockdockd_copy_dock_window_bounds(&probe->window_bounds)) {
        return false;
    }

    probe->has_window_bounds = true;
    probe->window_display = lockdockd_display_for_rect(probe->window_bounds);
    return true;
}

CGDirectDisplayID lockdockd_resolve_dock_probe(const LockDockdDockProbe *probe,
                                               bool allow_slow_fallback) {
    if (probe != NULL && probe->window_display != 0) {
        return probe->window_display;
    }

    if (!allow_slow_fallback) {
        return 0;
    }

    return lockdockd_get_dock_display_via_accessibility();
}

CGDirectDisplayID lockdockd_get_dock_display(void) {
    LockDockdDockProbe probe;

    lockdockd_capture_dock_probe(&probe);
    return lockdockd_resolve_dock_probe(&probe, true);
}
