#include <Unity/unity.h>

#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../src/lockdock_display.h"

typedef struct {
    CGDirectDisplayID display_id;
    CGRect bounds;
} LockDockTestDisplay;

static LockDockTestDisplay g_displays[LOCKDOCK_MAX_DISPLAYS];
static uint32_t g_display_count = 0;
static int g_active_display_query_count = 0;
static int g_display_bounds_query_count = 0;
static CGPoint g_event_location = {0};
static LockDockDockOrientation g_dock_orientation = LOCKDOCK_ORIENT_BOTTOM;

#include "../src/lockdock_locker.c"

static void lockdock_test_add_display(CGDirectDisplayID display_id, CGRect bounds) {
    TEST_ASSERT_LESS_THAN_UINT32(LOCKDOCK_MAX_DISPLAYS, g_display_count);
    g_displays[g_display_count].display_id = display_id;
    g_displays[g_display_count].bounds = bounds;
    g_display_count++;
}

static void lockdock_test_set_display_bounds(CGDirectDisplayID display_id,
                                             CGRect bounds) {
    for (uint32_t i = 0; i < g_display_count; i++) {
        if (g_displays[i].display_id == display_id) {
            g_displays[i].bounds = bounds;
            return;
        }
    }

    TEST_FAIL_MESSAGE("display not found");
}

uint32_t lockdock_get_active_displays(CGDirectDisplayID *displays,
                                      uint32_t max_displays) {
    uint32_t count = g_display_count;

    g_active_display_query_count++;
    if (count > max_displays) {
        count = max_displays;
    }

    if (displays != NULL) {
        for (uint32_t i = 0; i < count; i++) {
            displays[i] = g_displays[i].display_id;
        }
    }

    return count;
}

CGRect CGDisplayBounds(CGDirectDisplayID display_id) {
    g_display_bounds_query_count++;
    for (uint32_t i = 0; i < g_display_count; i++) {
        if (g_displays[i].display_id == display_id) {
            return g_displays[i].bounds;
        }
    }

    return CGRectZero;
}

CGPoint CGEventGetLocation(CGEventRef event) {
    (void)event;
    return g_event_location;
}

void lockdock_invalidate_dock_orientation_cache(void) {}

LockDockDockOrientation lockdock_get_dock_orientation(void) {
    return g_dock_orientation;
}

void setUp(void) {
    memset(g_displays, 0, sizeof(g_displays));
    g_display_count = 0;
    g_active_display_query_count = 0;
    g_display_bounds_query_count = 0;
    g_event_location = CGPointZero;
    g_dock_orientation = LOCKDOCK_ORIENT_BOTTOM;
    atomic_store(&g_locked_display, 0);

    pthread_mutex_lock(&g_display_cache_mutex);
    memset(g_display_cache, 0, sizeof(g_display_cache));
    g_display_cache_count = 0;
    pthread_mutex_unlock(&g_display_cache_mutex);
}

void tearDown(void) {}

static void test_event_callback_uses_cached_geometry_without_display_queries(void) {
    CGEventRef event = (CGEventRef)(uintptr_t)1;

    lockdock_test_add_display(10, CGRectMake(0, 0, 100, 100));
    lockdock_test_add_display(20, CGRectMake(100, 0, 100, 100));
    lockdock_locker_refresh_display_cache();
    g_active_display_query_count = 0;
    g_display_bounds_query_count = 0;

    atomic_store(&g_locked_display, 10);
    g_dock_orientation = LOCKDOCK_ORIENT_RIGHT;
    g_event_location = CGPointMake(198, 50);

    TEST_ASSERT_NULL(
        lockdock_locker_event_callback(NULL, kCGEventMouseMoved, event, NULL));
    TEST_ASSERT_EQUAL_INT(0, g_active_display_query_count);
    TEST_ASSERT_EQUAL_INT(0, g_display_bounds_query_count);
}

static void test_refresh_display_cache_updates_event_callback_geometry(void) {
    CGEventRef event = (CGEventRef)(uintptr_t)1;

    lockdock_test_add_display(10, CGRectMake(0, 0, 100, 100));
    lockdock_test_add_display(20, CGRectMake(100, 0, 100, 100));
    lockdock_locker_refresh_display_cache();
    lockdock_test_set_display_bounds(20, CGRectMake(300, 0, 100, 100));
    lockdock_locker_refresh_display_cache();
    g_active_display_query_count = 0;
    g_display_bounds_query_count = 0;

    atomic_store(&g_locked_display, 10);
    g_dock_orientation = LOCKDOCK_ORIENT_RIGHT;
    g_event_location = CGPointMake(398, 50);

    TEST_ASSERT_NULL(
        lockdock_locker_event_callback(NULL, kCGEventMouseMoved, event, NULL));
    TEST_ASSERT_EQUAL_INT(0, g_active_display_query_count);
    TEST_ASSERT_EQUAL_INT(0, g_display_bounds_query_count);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_event_callback_uses_cached_geometry_without_display_queries);
    RUN_TEST(test_refresh_display_cache_updates_event_callback_geometry);
    return UNITY_END();
}
