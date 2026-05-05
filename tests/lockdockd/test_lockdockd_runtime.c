#include <Unity/unity.h>

#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static CGDirectDisplayID g_active_displays[32];
static uint32_t g_active_display_count = 0;
static CGDirectDisplayID g_dock_display = 0;
static bool g_accessibility_trusted = false;
static CGDirectDisplayID g_builtin_display_id = 0;
static CGDirectDisplayID g_named_display_id = 0;
static bool g_has_platform_name = false;
static char g_platform_name[256];

#include "../../src/lockdockd/lockdockd_runtime.c"

uint32_t lockdockd_get_active_displays(CGDirectDisplayID *displays,
                                       uint32_t max_displays) {
    uint32_t count = g_active_display_count;

    if (count > max_displays) {
        count = max_displays;
    }

    if (displays != NULL) {
        for (uint32_t i = 0; i < count; i++) {
            displays[i] = g_active_displays[i];
        }
    }

    return count;
}

bool lockdockd_copy_display_name(CGDirectDisplayID display_id,
                                 char *buffer,
                                 size_t buffer_size) {
    if (!g_has_platform_name || display_id != g_named_display_id || buffer == NULL ||
        buffer_size == 0) {
        return false;
    }

    snprintf(buffer, buffer_size, "%s", g_platform_name);
    return true;
}

bool lockdockd_is_accessibility_trusted(void) {
    return g_accessibility_trusted;
}

CGDirectDisplayID lockdockd_get_dock_display(void) {
    return g_dock_display;
}

void lockdockd_invalidate_dock_orientation_cache(void) {}

LockDockdDockOrientation lockdockd_get_dock_orientation(void) {
    return LOCKDOCKD_ORIENT_BOTTOM;
}

void lockdockd_reset_dock_probe(LockDockdDockProbe *probe) {
    if (probe != NULL) {
        memset(probe, 0, sizeof(*probe));
    }
}

bool lockdockd_capture_dock_probe(LockDockdDockProbe *probe) {
    (void)probe;
    return false;
}

CGDirectDisplayID lockdockd_resolve_dock_probe(const LockDockdDockProbe *probe,
                                               bool allow_slow_fallback) {
    (void)probe;
    (void)allow_slow_fallback;
    return 0;
}

LockDockdSafeSegment lockdockd_find_safe_edge_segment(
    CGDirectDisplayID display_id,
    LockDockdDockOrientation edge) {
    LockDockdSafeSegment segment = {0, 100, 100, 50};

    (void)display_id;
    (void)edge;
    return segment;
}

boolean_t CGDisplayIsBuiltin(CGDirectDisplayID display_id) {
    return display_id == g_builtin_display_id;
}

CGRect CGDisplayBounds(CGDirectDisplayID display_id) {
    if (display_id == 0) {
        return CGRectZero;
    }

    return CGRectMake(0, 0, 100, 100);
}

CGEventRef CGEventCreate(CGEventSourceRef source) {
    (void)source;
    return NULL;
}

CGPoint CGEventGetLocation(CGEventRef event) {
    (void)event;
    return CGPointZero;
}

CGEventRef CGEventCreateMouseEvent(CGEventSourceRef source,
                                   CGEventType mouse_type,
                                   CGPoint mouse_cursor_position,
                                   CGMouseButton mouse_button) {
    (void)source;
    (void)mouse_type;
    (void)mouse_cursor_position;
    (void)mouse_button;
    return NULL;
}

void CGEventPost(CGEventTapLocation tap, CGEventRef event) {
    (void)tap;
    (void)event;
}

void CGEventSetIntegerValueField(CGEventRef event,
                                 CGEventField field,
                                 int64_t value) {
    (void)event;
    (void)field;
    (void)value;
}

CGError CGAssociateMouseAndMouseCursorPosition(boolean_t connected) {
    (void)connected;
    return kCGErrorSuccess;
}

CGError CGWarpMouseCursorPosition(CGPoint new_cursor_position) {
    (void)new_cursor_position;
    return kCGErrorSuccess;
}

CGEventSourceRef CGEventSourceCreate(CGEventSourceStateID state_id) {
    (void)state_id;
    return NULL;
}

void CGEventSourceSetLocalEventsSuppressionInterval(CGEventSourceRef source,
                                                    CFTimeInterval seconds) {
    (void)source;
    (void)seconds;
}

void setUp(void) {
    memset(g_active_displays, 0, sizeof(g_active_displays));
    g_active_display_count = 0;
    g_dock_display = 0;
    g_accessibility_trusted = false;
    g_builtin_display_id = 0;
    g_named_display_id = 0;
    g_has_platform_name = false;
    memset(g_platform_name, 0, sizeof(g_platform_name));
}

void tearDown(void) {}

static void test_copy_display_label_prefers_builtin_name(void) {
    char label[LOCKDOCKD_DISPLAY_NAME_BUFFER_SIZE];

    g_builtin_display_id = 12;
    TEST_ASSERT_TRUE(lockdockd_copy_display_label(12, label, sizeof(label)));
    TEST_ASSERT_EQUAL_STRING("Built-in Display", label);
}

static void test_copy_display_label_uses_platform_name_before_fallback(void) {
    char label[LOCKDOCKD_DISPLAY_NAME_BUFFER_SIZE];

    g_named_display_id = 33;
    g_has_platform_name = true;
    snprintf(g_platform_name, sizeof(g_platform_name), "Studio Display");

    TEST_ASSERT_TRUE(lockdockd_copy_display_label(33, label, sizeof(label)));
    TEST_ASSERT_EQUAL_STRING("Studio Display", label);

    TEST_ASSERT_TRUE(lockdockd_copy_display_label(44, label, sizeof(label)));
    TEST_ASSERT_EQUAL_STRING("Display-44", label);
}

static void test_status_index_for_display_finds_matching_display(void) {
    LockDockdStatus status = {0};

    status.displays[0] = 10;
    status.displays[1] = 20;
    status.display_count = 2;

    TEST_ASSERT_EQUAL_INT(1, lockdockd_status_index_for_display(&status, 20));
    TEST_ASSERT_EQUAL_INT(-1, lockdockd_status_index_for_display(&status, 99));
    TEST_ASSERT_EQUAL_INT(-1, lockdockd_status_index_for_display(NULL, 20));
}

static void test_query_status_reports_current_dock_display(void) {
    LockDockdStatus status;
    char error[LOCKDOCKD_ERROR_BUFFER_SIZE];

    g_active_displays[0] = 100;
    g_active_displays[1] = 200;
    g_active_display_count = 2;
    g_dock_display = 200;

    TEST_ASSERT_TRUE(lockdockd_query_status(&status, error, sizeof(error)));
    TEST_ASSERT_EQUAL_UINT32(2, status.display_count);
    TEST_ASSERT_EQUAL_UINT32(100, status.displays[0]);
    TEST_ASSERT_EQUAL_UINT32(200, status.displays[1]);
    TEST_ASSERT_EQUAL_INT(1, status.location_index);
}

static void test_query_status_distinguishes_accessibility_error_states(void) {
    LockDockdStatus status;
    char error[LOCKDOCKD_ERROR_BUFFER_SIZE];

    g_active_displays[0] = 300;
    g_active_display_count = 1;
    g_dock_display = 0;
    g_accessibility_trusted = false;

    TEST_ASSERT_FALSE(lockdockd_query_status(&status, error, sizeof(error)));
    TEST_ASSERT_NOT_NULL(strstr(error, "Accessibility permission is not granted"));

    g_accessibility_trusted = true;
    TEST_ASSERT_FALSE(lockdockd_query_status(&status, error, sizeof(error)));
    TEST_ASSERT_EQUAL_STRING("Could not determine current Dock display", error);
}

static void test_query_status_rejects_dock_display_outside_active_list(void) {
    LockDockdStatus status;
    char error[LOCKDOCKD_ERROR_BUFFER_SIZE];

    g_active_displays[0] = 400;
    g_active_display_count = 1;
    g_dock_display = 999;

    TEST_ASSERT_FALSE(lockdockd_query_status(&status, error, sizeof(error)));
    TEST_ASSERT_NOT_NULL(
        strstr(error, "Dock display 999 is not part of the active display list"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_copy_display_label_prefers_builtin_name);
    RUN_TEST(test_copy_display_label_uses_platform_name_before_fallback);
    RUN_TEST(test_status_index_for_display_finds_matching_display);
    RUN_TEST(test_query_status_reports_current_dock_display);
    RUN_TEST(test_query_status_distinguishes_accessibility_error_states);
    RUN_TEST(test_query_status_rejects_dock_display_outside_active_list);
    return UNITY_END();
}
