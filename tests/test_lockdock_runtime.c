#include <Unity/unity.h>

#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../src/lockdock_platform.h"
#include "../src/lockdock_runtime.h"

static CGDirectDisplayID g_active_displays[32];
static uint32_t g_active_display_count = 0;
static CGDirectDisplayID g_dock_display = 0;
static bool g_accessibility_trusted = false;
static CGDirectDisplayID g_builtin_display_id = 0;
static CGDirectDisplayID g_named_display_id = 0;
static bool g_has_platform_name = false;
static char g_platform_name[256];
static LockDockDockOrientation g_dock_orientation = lockdock_ORIENT_BOTTOM;
static LockDockSafeSegment g_safe_segment = {0, 100, 100, 50};
static bool g_edge_point_has_contact = false;
static LockDockDockProbe g_dock_probe_sequence[4];
static size_t g_dock_probe_sequence_count = 0;
static size_t g_dock_probe_sequence_index = 0;
static CGPoint g_warp_positions[64];
static size_t g_warp_position_count = 0;

typedef struct {
    CGDirectDisplayID display_id;
    CGRect bounds;
} LockDockTestBounds;

static LockDockTestBounds g_display_bounds[8];
static size_t g_display_bounds_count = 0;

#include "../src/lockdock_runtime.c"

static void lockdock_test_set_display_bounds(CGDirectDisplayID display_id,
                                             CGRect bounds) {
    TEST_ASSERT_TRUE(g_display_bounds_count <
                     (sizeof(g_display_bounds) / sizeof(g_display_bounds[0])));
    g_display_bounds[g_display_bounds_count].display_id = display_id;
    g_display_bounds[g_display_bounds_count].bounds = bounds;
    g_display_bounds_count++;
}

uint32_t lockdock_get_active_displays(CGDirectDisplayID *displays,
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

bool lockdock_copy_display_name(CGDirectDisplayID display_id,
                                char *buffer,
                                size_t buffer_size) {
    if (!g_has_platform_name || display_id != g_named_display_id || buffer == NULL ||
        buffer_size == 0) {
        return false;
    }

    snprintf(buffer, buffer_size, "%s", g_platform_name);
    return true;
}

bool lockdock_is_accessibility_trusted(void) {
    return g_accessibility_trusted;
}

CGDirectDisplayID lockdock_get_dock_display(void) {
    return g_dock_display;
}

void lockdock_invalidate_dock_orientation_cache(void) {}

LockDockDockOrientation lockdock_get_dock_orientation(void) {
    return g_dock_orientation;
}

void lockdock_reset_dock_probe(LockDockDockProbe *probe) {
    if (probe != NULL) {
        memset(probe, 0, sizeof(*probe));
    }
}

bool lockdock_capture_dock_probe(LockDockDockProbe *probe) {
    size_t index;

    if (g_dock_probe_sequence_count == 0) {
        return false;
    }

    index = g_dock_probe_sequence_index;
    if (index >= g_dock_probe_sequence_count) {
        index = g_dock_probe_sequence_count - 1;
    }

    if (probe != NULL) {
        *probe = g_dock_probe_sequence[index];
    }

    if (g_dock_probe_sequence_index + 1 < g_dock_probe_sequence_count) {
        g_dock_probe_sequence_index++;
    }

    return g_dock_probe_sequence[index].has_window_bounds;
}

CGDirectDisplayID lockdock_resolve_dock_probe(const LockDockDockProbe *probe,
                                              bool allow_slow_fallback) {
    (void)allow_slow_fallback;

    if (probe != NULL && probe->window_display != 0) {
        return probe->window_display;
    }

    return 0;
}

LockDockSafeSegment lockdock_find_safe_edge_segment(CGDirectDisplayID display_id,
                                                    LockDockDockOrientation edge) {
    (void)display_id;
    (void)edge;
    return g_safe_segment;
}

bool lockdock_edge_point_has_contact(CGDirectDisplayID display_id,
                                     LockDockDockOrientation edge,
                                     CGFloat point_along_edge) {
    (void)display_id;
    (void)edge;
    (void)point_along_edge;
    return g_edge_point_has_contact;
}

CGDirectDisplayID lockdock_find_display_at_point(CGPoint point) {
    for (size_t i = 0; i < g_display_bounds_count; i++) {
        CGRect bounds = g_display_bounds[i].bounds;

        if (point.x >= bounds.origin.x &&
            point.x < bounds.origin.x + bounds.size.width &&
            point.y >= bounds.origin.y &&
            point.y < bounds.origin.y + bounds.size.height) {
            return g_display_bounds[i].display_id;
        }
    }

    return 0;
}

boolean_t CGDisplayIsBuiltin(CGDirectDisplayID display_id) {
    return display_id == g_builtin_display_id;
}

CGRect CGDisplayBounds(CGDirectDisplayID display_id) {
    for (size_t i = 0; i < g_display_bounds_count; i++) {
        if (g_display_bounds[i].display_id == display_id) {
            return g_display_bounds[i].bounds;
        }
    }

    return CGRectZero;
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
    if (g_warp_position_count <
        (sizeof(g_warp_positions) / sizeof(g_warp_positions[0]))) {
        g_warp_positions[g_warp_position_count] = new_cursor_position;
        g_warp_position_count++;
    }

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

int usleep(useconds_t usec) {
    (void)usec;
    return 0;
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
    g_dock_orientation = lockdock_ORIENT_BOTTOM;
    g_safe_segment = (LockDockSafeSegment){0, 100, 100, 50};
    g_edge_point_has_contact = false;
    memset(g_dock_probe_sequence, 0, sizeof(g_dock_probe_sequence));
    g_dock_probe_sequence_count = 0;
    g_dock_probe_sequence_index = 0;
    memset(g_warp_positions, 0, sizeof(g_warp_positions));
    g_warp_position_count = 0;
    memset(g_display_bounds, 0, sizeof(g_display_bounds));
    g_display_bounds_count = 0;
}

void tearDown(void) {}

static void test_copy_display_label_prefers_builtin_name(void) {
    char label[lockdock_DISPLAY_NAME_BUFFER_SIZE];

    g_builtin_display_id = 12;
    TEST_ASSERT_TRUE(lockdock_copy_display_label(12, label, sizeof(label)));
    TEST_ASSERT_EQUAL_STRING("Built-in Display", label);
}

static void test_copy_display_label_uses_platform_name_before_fallback(void) {
    char label[lockdock_DISPLAY_NAME_BUFFER_SIZE];

    g_named_display_id = 33;
    g_has_platform_name = true;
    snprintf(g_platform_name, sizeof(g_platform_name), "Studio Display");

    TEST_ASSERT_TRUE(lockdock_copy_display_label(33, label, sizeof(label)));
    TEST_ASSERT_EQUAL_STRING("Studio Display", label);

    TEST_ASSERT_TRUE(lockdock_copy_display_label(44, label, sizeof(label)));
    TEST_ASSERT_EQUAL_STRING("Display-44", label);
}

static void test_status_index_for_display_finds_matching_display(void) {
    LockDockStatus status = {0};

    status.displays[0] = 10;
    status.displays[1] = 20;
    status.display_count = 2;

    TEST_ASSERT_EQUAL_INT(1, lockdock_status_index_for_display(&status, 20));
    TEST_ASSERT_EQUAL_INT(-1, lockdock_status_index_for_display(&status, 99));
    TEST_ASSERT_EQUAL_INT(-1, lockdock_status_index_for_display(NULL, 20));
}

static void test_query_status_reports_current_dock_display(void) {
    LockDockStatus status;
    char error[lockdock_ERROR_BUFFER_SIZE];

    g_active_displays[0] = 100;
    g_active_displays[1] = 200;
    g_active_display_count = 2;
    g_dock_display = 200;

    TEST_ASSERT_TRUE(lockdock_query_status(&status, error, sizeof(error)));
    TEST_ASSERT_EQUAL_UINT32(2, status.display_count);
    TEST_ASSERT_EQUAL_UINT32(100, status.displays[0]);
    TEST_ASSERT_EQUAL_UINT32(200, status.displays[1]);
    TEST_ASSERT_EQUAL_INT(1, status.location_index);
}

static void test_query_status_distinguishes_accessibility_error_states(void) {
    LockDockStatus status;
    char error[lockdock_ERROR_BUFFER_SIZE];

    g_active_displays[0] = 300;
    g_active_display_count = 1;
    g_dock_display = 0;
    g_accessibility_trusted = false;

    TEST_ASSERT_FALSE(lockdock_query_status(&status, error, sizeof(error)));
    TEST_ASSERT_NOT_NULL(strstr(error, "Accessibility permission is not granted"));

    g_accessibility_trusted = true;
    TEST_ASSERT_FALSE(lockdock_query_status(&status, error, sizeof(error)));
    TEST_ASSERT_EQUAL_STRING("Could not determine current Dock display", error);
}

static void test_query_status_rejects_dock_display_outside_active_list(void) {
    LockDockStatus status;
    char error[lockdock_ERROR_BUFFER_SIZE];

    g_active_displays[0] = 400;
    g_active_display_count = 1;
    g_dock_display = 999;

    TEST_ASSERT_FALSE(lockdock_query_status(&status, error, sizeof(error)));
    TEST_ASSERT_NOT_NULL(
        strstr(error, "Dock display 999 is not part of the active display list"));
}

static void test_relocate_display_uses_right_edge_when_bottom_corner_has_no_contact(
    void) {
    char error[lockdock_ERROR_BUFFER_SIZE];

    g_active_displays[0] = 11;
    g_active_displays[1] = 12;
    g_active_display_count = 2;
    g_builtin_display_id = 12;
    g_dock_orientation = lockdock_ORIENT_BOTTOM;
    g_safe_segment = (LockDockSafeSegment){2000, 3000, 1000, 2500};
    g_edge_point_has_contact = false;

    lockdock_test_set_display_bounds(11, CGRectMake(0, 0, 1000, 600));
    lockdock_test_set_display_bounds(12, CGRectMake(2000, 0, 1000, 600));

    g_dock_probe_sequence[0] = (LockDockDockProbe){
        .has_window_bounds = true,
        .window_bounds = CGRectMake(2350, 560, 300, 40),
        .window_display = 12,
    };
    g_dock_probe_sequence_count = 1;

    TEST_ASSERT_TRUE(lockdock_relocate_display(12, error, sizeof(error)));
    TEST_ASSERT_TRUE(g_warp_position_count > 0);
    TEST_ASSERT_EQUAL_INT(2990, (int)g_warp_positions[0].x);
    TEST_ASSERT_EQUAL_INT(599, (int)g_warp_positions[0].y);
}

static void test_relocate_display_falls_back_to_safe_segment_when_corner_has_contact(
    void) {
    char error[lockdock_ERROR_BUFFER_SIZE];

    g_active_displays[0] = 12;
    g_active_display_count = 1;
    g_builtin_display_id = 12;
    g_dock_orientation = lockdock_ORIENT_BOTTOM;
    g_safe_segment = (LockDockSafeSegment){2000, 3000, 1000, 2500};
    g_edge_point_has_contact = true;

    lockdock_test_set_display_bounds(12, CGRectMake(2000, 0, 1000, 600));

    g_dock_probe_sequence[0] = (LockDockDockProbe){
        .has_window_bounds = true,
        .window_bounds = CGRectMake(2350, 560, 300, 40),
        .window_display = 12,
    };
    g_dock_probe_sequence_count = 1;

    TEST_ASSERT_TRUE(lockdock_relocate_display(12, error, sizeof(error)));
    TEST_ASSERT_TRUE(g_warp_position_count > 0);
    TEST_ASSERT_EQUAL_INT(2500, (int)g_warp_positions[0].x);
    TEST_ASSERT_EQUAL_INT(599, (int)g_warp_positions[0].y);
}

static void test_relocate_display_uses_bottom_edge_when_side_corner_has_no_contact(
    void) {
    char error[lockdock_ERROR_BUFFER_SIZE];

    g_active_displays[0] = 22;
    g_active_display_count = 1;
    g_dock_orientation = lockdock_ORIENT_LEFT;
    g_safe_segment = (LockDockSafeSegment){0, 600, 600, 300};
    g_edge_point_has_contact = false;

    lockdock_test_set_display_bounds(22, CGRectMake(2000, 0, 1000, 600));

    g_dock_probe_sequence[0] = (LockDockDockProbe){
        .has_window_bounds = true,
        .window_bounds = CGRectMake(2000, 100, 40, 300),
        .window_display = 22,
    };
    g_dock_probe_sequence_count = 1;

    TEST_ASSERT_TRUE(lockdock_relocate_display(22, error, sizeof(error)));
    TEST_ASSERT_TRUE(g_warp_position_count > 0);
    TEST_ASSERT_EQUAL_INT(2001, (int)g_warp_positions[0].x);
    TEST_ASSERT_EQUAL_INT(590, (int)g_warp_positions[0].y);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_copy_display_label_prefers_builtin_name);
    RUN_TEST(test_copy_display_label_uses_platform_name_before_fallback);
    RUN_TEST(test_status_index_for_display_finds_matching_display);
    RUN_TEST(test_query_status_reports_current_dock_display);
    RUN_TEST(test_query_status_distinguishes_accessibility_error_states);
    RUN_TEST(test_query_status_rejects_dock_display_outside_active_list);
    RUN_TEST(
        test_relocate_display_uses_right_edge_when_bottom_corner_has_no_contact);
    RUN_TEST(
        test_relocate_display_falls_back_to_safe_segment_when_corner_has_contact);
    RUN_TEST(test_relocate_display_uses_bottom_edge_when_side_corner_has_no_contact);
    return UNITY_END();
}
