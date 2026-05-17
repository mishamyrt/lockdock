#include "lockdock_locker.h"

#include "lockdock_display.h"
#include "lockdock_platform.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LOCKDOCK_LOCKER_ERROR_BUFFER_SIZE 256

typedef struct {
    CGDirectDisplayID display_id;
    CGRect bounds;
} LockDockCachedDisplay;

static _Atomic uint32_t g_locked_display = 0;
static double g_lock_edge_zone = 4.0;
static pthread_mutex_t g_display_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static LockDockCachedDisplay g_display_cache[LOCKDOCK_MAX_DISPLAYS];
static uint32_t g_display_cache_count = 0;
static CFMachPortRef g_event_tap = NULL;
static CFRunLoopSourceRef g_event_source = NULL;
static CFRunLoopRef g_event_run_loop = NULL;
static pthread_t g_event_thread;
static pthread_mutex_t g_event_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_event_thread_cond = PTHREAD_COND_INITIALIZER;
static bool g_event_thread_starting = false;
static bool g_event_thread_running = false;
static bool g_event_thread_joinable = false;
static char g_event_thread_error[LOCKDOCK_LOCKER_ERROR_BUFFER_SIZE];

static void lockdock_locker_enable_tap(void) {
    if (g_event_tap != NULL) {
        CGEventTapEnable(g_event_tap, true);
    }
}

void lockdock_locker_refresh_display_cache(void) {
    CGDirectDisplayID displays[LOCKDOCK_MAX_DISPLAYS];
    LockDockCachedDisplay cache[LOCKDOCK_MAX_DISPLAYS];
    uint32_t count;

    count = lockdock_get_active_displays(displays, LOCKDOCK_MAX_DISPLAYS);
    for (uint32_t i = 0; i < count; i++) {
        cache[i].display_id = displays[i];
        cache[i].bounds = CGDisplayBounds(displays[i]);
    }

    pthread_mutex_lock(&g_display_cache_mutex);
    memcpy(g_display_cache, cache, sizeof(cache[0]) * count);
    g_display_cache_count = count;
    pthread_mutex_unlock(&g_display_cache_mutex);
}

static bool lockdock_locker_copy_display_at_point(CGPoint point,
                                                  CGDirectDisplayID *display_id_out,
                                                  CGRect *bounds_out) {
    bool found = false;

    if (display_id_out == NULL || bounds_out == NULL) {
        return false;
    }

    pthread_mutex_lock(&g_display_cache_mutex);
    for (uint32_t i = 0; i < g_display_cache_count; i++) {
        CGRect bounds = g_display_cache[i].bounds;

        if (point.x < bounds.origin.x ||
            point.x >= bounds.origin.x + bounds.size.width ||
            point.y < bounds.origin.y ||
            point.y >= bounds.origin.y + bounds.size.height) {
            continue;
        }

        *display_id_out = g_display_cache[i].display_id;
        *bounds_out = bounds;
        found = true;
        break;
    }
    pthread_mutex_unlock(&g_display_cache_mutex);

    return found;
}

static CGFloat lockdock_distance_from_dock_edge(
    CGPoint point,
    CGRect bounds,
    LockDockDockOrientation orientation) {
    if (orientation == LOCKDOCK_ORIENT_LEFT) {
        return point.x - bounds.origin.x;
    }

    if (orientation == LOCKDOCK_ORIENT_RIGHT) {
        return (bounds.origin.x + bounds.size.width) - point.x;
    }

    return (bounds.origin.y + bounds.size.height) - point.y;
}

static CGEventRef lockdock_locker_event_callback(CGEventTapProxy proxy,
                                                 CGEventType type,
                                                 CGEventRef event,
                                                 void *user_info) {
    CGPoint point;
    CGDirectDisplayID current_display;
    CGDirectDisplayID locked_display;
    CGRect bounds;
    CGFloat distance;
    LockDockDockOrientation orientation;

    (void)proxy;
    (void)user_info;

    if (type == kCGEventTapDisabledByTimeout ||
        type == kCGEventTapDisabledByUserInput) {
        lockdock_locker_enable_tap();
        return event;
    }

    if (type != kCGEventMouseMoved && type != kCGEventLeftMouseDragged &&
        type != kCGEventRightMouseDragged && type != kCGEventOtherMouseDragged) {
        return event;
    }

    locked_display = (CGDirectDisplayID)atomic_load(&g_locked_display);
    if (locked_display == 0) {
        return event;
    }

    point = CGEventGetLocation(event);
    if (!lockdock_locker_copy_display_at_point(point, &current_display, &bounds) ||
        current_display == locked_display) {
        return event;
    }

    orientation = lockdock_get_dock_orientation();
    distance = lockdock_distance_from_dock_edge(point, bounds, orientation);

    if (distance < 0 || distance > g_lock_edge_zone) {
        return event;
    }

    return NULL;
}

static void lockdock_set_error(char *buffer,
                               size_t buffer_size,
                               const char *message) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", message);
}

static void lockdock_locker_set_thread_error(const char *message) {
    snprintf(g_event_thread_error, sizeof(g_event_thread_error), "%s", message);
}

static void lockdock_locker_signal_thread_state(void) {
    pthread_cond_broadcast(&g_event_thread_cond);
}

static void lockdock_locker_publish_thread_ready(CFRunLoopRef run_loop) {
    pthread_mutex_lock(&g_event_thread_mutex);
    g_event_run_loop = run_loop;
    g_event_thread_running = true;
    g_event_thread_starting = false;
    g_event_thread_error[0] = '\0';
    lockdock_locker_signal_thread_state();
    pthread_mutex_unlock(&g_event_thread_mutex);
}

static void lockdock_locker_publish_thread_failure(const char *message) {
    pthread_mutex_lock(&g_event_thread_mutex);
    g_event_thread_running = false;
    g_event_thread_starting = false;
    lockdock_locker_set_thread_error(message);
    lockdock_locker_signal_thread_state();
    pthread_mutex_unlock(&g_event_thread_mutex);
}

static void lockdock_locker_publish_thread_stopped(void) {
    pthread_mutex_lock(&g_event_thread_mutex);
    g_event_tap = NULL;
    g_event_source = NULL;
    g_event_run_loop = NULL;
    g_event_thread_running = false;
    lockdock_locker_signal_thread_state();
    pthread_mutex_unlock(&g_event_thread_mutex);
}

static void *lockdock_locker_thread_main(void *user_info) {
    CGEventMask mask;
    CFRunLoopRef run_loop;

    (void)user_info;

    mask = CGEventMaskBit(kCGEventMouseMoved) |
           CGEventMaskBit(kCGEventLeftMouseDragged) |
           CGEventMaskBit(kCGEventRightMouseDragged) |
           CGEventMaskBit(kCGEventOtherMouseDragged);

    g_event_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                   kCGEventTapOptionDefault, mask,
                                   lockdock_locker_event_callback, NULL);
    if (g_event_tap == NULL) {
        lockdock_locker_publish_thread_failure(
            "Failed to create event tap. Grant Accessibility permission in "
            "System Settings");
        return NULL;
    }

    g_event_source =
        CFMachPortCreateRunLoopSource(kCFAllocatorDefault, g_event_tap, 0);
    if (g_event_source == NULL) {
        CFRelease(g_event_tap);
        g_event_tap = NULL;
        lockdock_locker_publish_thread_failure("Failed to create event tap source");
        return NULL;
    }

    run_loop = CFRunLoopGetCurrent();
    CFRunLoopAddSource(run_loop, g_event_source, kCFRunLoopCommonModes);
    lockdock_locker_enable_tap();
    lockdock_locker_publish_thread_ready(run_loop);

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

    lockdock_locker_publish_thread_stopped();
    return NULL;
}

static void lockdock_locker_wait_for_thread_start(void) {
    while (g_event_thread_starting) {
        pthread_cond_wait(&g_event_thread_cond, &g_event_thread_mutex);
    }
}

static void lockdock_locker_join_thread(pthread_t thread) {
    pthread_join(thread, NULL);
}

static bool lockdock_locker_ensure_tap(char *error, size_t error_size) {
    pthread_t stale_thread;
    bool has_stale_thread = false;

    pthread_mutex_lock(&g_event_thread_mutex);
    lockdock_locker_wait_for_thread_start();

    if (g_event_thread_running) {
        pthread_mutex_unlock(&g_event_thread_mutex);
        return true;
    }

    if (g_event_thread_joinable) {
        stale_thread = g_event_thread;
        g_event_thread_joinable = false;
        has_stale_thread = true;
    }

    g_event_thread_starting = true;
    g_event_thread_error[0] = '\0';
    pthread_mutex_unlock(&g_event_thread_mutex);

    if (has_stale_thread) {
        lockdock_locker_join_thread(stale_thread);
    }

    if (pthread_create(&g_event_thread, NULL, lockdock_locker_thread_main, NULL) !=
        0) {
        pthread_mutex_lock(&g_event_thread_mutex);
        g_event_thread_starting = false;
        pthread_mutex_unlock(&g_event_thread_mutex);
        snprintf(error, error_size, "Failed to start locker event thread");
        return false;
    }

    pthread_mutex_lock(&g_event_thread_mutex);
    g_event_thread_joinable = true;
    lockdock_locker_wait_for_thread_start();

    if (g_event_thread_running) {
        pthread_mutex_unlock(&g_event_thread_mutex);
        return true;
    }

    lockdock_set_error(error, error_size, g_event_thread_error);
    stale_thread = g_event_thread;
    g_event_thread_joinable = false;
    pthread_mutex_unlock(&g_event_thread_mutex);
    lockdock_locker_join_thread(stale_thread);
    return false;
}

static void lockdock_locker_stop_tap(void) {
    pthread_t thread;
    CFRunLoopRef run_loop = NULL;
    bool join_thread = false;

    pthread_mutex_lock(&g_event_thread_mutex);
    lockdock_locker_wait_for_thread_start();

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

    pthread_mutex_unlock(&g_event_thread_mutex);

    if (join_thread) {
        lockdock_locker_join_thread(thread);
    }
}

bool lockdock_locker_set_target(CGDirectDisplayID display_id,
                                char *error,
                                size_t error_size) {
    if (!lockdock_locker_ensure_tap(error, error_size)) {
        return false;
    }

    lockdock_locker_refresh_display_cache();
    lockdock_invalidate_dock_orientation_cache();
    atomic_store(&g_locked_display, display_id);
    return true;
}

void lockdock_locker_clear_target(void) {
    atomic_store(&g_locked_display, 0);
    lockdock_locker_stop_tap();
}

CGDirectDisplayID lockdock_locker_get_target(void) {
    return (CGDirectDisplayID)atomic_load(&g_locked_display);
}

void lockdock_locker_shutdown(void) {
    lockdock_locker_clear_target();
}
