#include "mouse.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <pthread.h>
#include <stdio.h>

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

bool lockdock_mouse_copy_location(LockDockMousePoint *point_out) {
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

void *lockdock_mouse_event_source_create(void) {
    return CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
}

void lockdock_mouse_event_source_set_suppression_interval(void *source,
                                                          double interval) {
    if (source != NULL) {
        CGEventSourceSetLocalEventsSuppressionInterval((CGEventSourceRef)source,
                                                       interval);
    }
}

void lockdock_mouse_release(void *object) {
    if (object != NULL) {
        CFRelease(object);
    }
}

void lockdock_mouse_warp(LockDockMousePoint point) {
    CGWarpMouseCursorPosition(CGPointMake(point.x, point.y));
}

void lockdock_mouse_post_moved(void *source, LockDockMousePoint point) {
    CGEventRef event = CGEventCreateMouseEvent((CGEventSourceRef)source,
                                               kCGEventMouseMoved,
                                               CGPointMake(point.x, point.y),
                                               kCGMouseButtonLeft);

    if (event != NULL) {
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }
}

void lockdock_mouse_post_delta(void *source,
                               LockDockMousePoint point,
                               int64_t delta_x,
                               int64_t delta_y) {
    CGEventRef event = CGEventCreateMouseEvent((CGEventSourceRef)source,
                                               kCGEventMouseMoved,
                                               CGPointMake(point.x, point.y),
                                               kCGMouseButtonLeft);

    if (event == NULL) {
        return;
    }

    if (delta_x != 0) {
        CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, delta_x);
    }
    if (delta_y != 0) {
        CGEventSetIntegerValueField(event, kCGMouseEventDeltaY, delta_y);
    }

    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

static void lockdock_mouse_event_tap_enable(void) {
    if (g_event_tap != NULL) {
        CGEventTapEnable(g_event_tap, true);
    }
}

static CGEventRef lockdock_mouse_event_callback(CGEventTapProxy proxy,
                                                CGEventType type,
                                                CGEventRef event,
                                                void *user_info) {
    CGPoint point;
    int kind = LOCKDOCK_MOUSE_EVENT_OTHER;

    (void)proxy;
    (void)user_info;

    if (type == kCGEventTapDisabledByTimeout ||
        type == kCGEventTapDisabledByUserInput) {
        lockdock_mouse_event_tap_enable();
        return event;
    }

    if (type == kCGEventMouseMoved) {
        kind = LOCKDOCK_MOUSE_EVENT_MOVED;
    } else if (type == kCGEventLeftMouseDragged ||
               type == kCGEventRightMouseDragged ||
               type == kCGEventOtherMouseDragged) {
        kind = LOCKDOCK_MOUSE_EVENT_DRAGGED;
    } else {
        return event;
    }

    point = CGEventGetLocation(event);
    if (lockdock_mouse_should_suppress_event(kind, point.x, point.y)) {
        return NULL;
    }

    return event;
}

static void lockdock_mouse_publish_started(CFRunLoopRef run_loop) {
    pthread_mutex_lock(&g_event_mutex);
    g_event_run_loop = run_loop;
    g_event_thread_running = true;
    g_event_thread_starting = false;
    g_event_error[0] = '\0';
    pthread_cond_broadcast(&g_event_cond);
    pthread_mutex_unlock(&g_event_mutex);
}

static void lockdock_mouse_publish_failed(const char *message) {
    pthread_mutex_lock(&g_event_mutex);
    g_event_thread_running = false;
    g_event_thread_starting = false;
    snprintf(g_event_error, sizeof(g_event_error), "%s", message);
    pthread_cond_broadcast(&g_event_cond);
    pthread_mutex_unlock(&g_event_mutex);
}

static void lockdock_mouse_publish_stopped(void) {
    pthread_mutex_lock(&g_event_mutex);
    g_event_tap = NULL;
    g_event_source = NULL;
    g_event_run_loop = NULL;
    g_event_thread_running = false;
    pthread_cond_broadcast(&g_event_cond);
    pthread_mutex_unlock(&g_event_mutex);
}

static void *lockdock_mouse_event_thread(void *context) {
    CGEventMask mask;
    CFRunLoopRef run_loop;

    (void)context;

    mask = CGEventMaskBit(kCGEventMouseMoved) |
           CGEventMaskBit(kCGEventLeftMouseDragged) |
           CGEventMaskBit(kCGEventRightMouseDragged) |
           CGEventMaskBit(kCGEventOtherMouseDragged);

    g_event_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                   kCGEventTapOptionDefault, mask,
                                   lockdock_mouse_event_callback, NULL);
    if (g_event_tap == NULL) {
        lockdock_mouse_publish_failed(
            "Failed to create event tap. Grant Accessibility permission in System Settings");
        return NULL;
    }

    g_event_source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault,
                                                   g_event_tap, 0);
    if (g_event_source == NULL) {
        CFRelease(g_event_tap);
        g_event_tap = NULL;
        lockdock_mouse_publish_failed("Failed to create event tap source");
        return NULL;
    }

    run_loop = CFRunLoopGetCurrent();
    CFRunLoopAddSource(run_loop, g_event_source, kCFRunLoopCommonModes);
    lockdock_mouse_event_tap_enable();
    lockdock_mouse_publish_started(run_loop);

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

    lockdock_mouse_publish_stopped();
    return NULL;
}

static void lockdock_mouse_wait_for_start(void) {
    while (g_event_thread_starting) {
        pthread_cond_wait(&g_event_cond, &g_event_mutex);
    }
}

bool lockdock_mouse_start_event_tap(char *error, size_t error_size) {
    pthread_t stale_thread;
    bool has_stale_thread = false;

    pthread_mutex_lock(&g_event_mutex);
    lockdock_mouse_wait_for_start();

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

    if (pthread_create(&g_event_thread, NULL, lockdock_mouse_event_thread, NULL) !=
        0) {
        pthread_mutex_lock(&g_event_mutex);
        g_event_thread_starting = false;
        pthread_mutex_unlock(&g_event_mutex);
        snprintf(error, error_size, "Failed to start mouse event thread");
        return false;
    }

    pthread_mutex_lock(&g_event_mutex);
    g_event_thread_joinable = true;
    lockdock_mouse_wait_for_start();
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

void lockdock_mouse_stop_event_tap(void) {
    pthread_t thread;
    CFRunLoopRef run_loop = NULL;
    bool join_thread = false;

    pthread_mutex_lock(&g_event_mutex);
    lockdock_mouse_wait_for_start();

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
