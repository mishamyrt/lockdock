#include <Unity/unity.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    CGDirectDisplayID display_id;
    bool is_builtin;
    uint32_t vendor_number;
    uint32_t model_number;
    uint32_t serial_number;
    const char *uuid;
    CGRect bounds;
} LockDockTestDisplay;

static LockDockTestDisplay g_displays[8];
static uint32_t g_display_count = 0;

static void lockdock_test_add_display(CGDirectDisplayID display_id,
                                      CGRect bounds,
                                      bool is_builtin,
                                      uint32_t vendor_number,
                                      uint32_t model_number,
                                      uint32_t serial_number,
                                      const char *uuid) {
    TEST_ASSERT_LESS_THAN_UINT32(
        (uint32_t)(sizeof(g_displays) / sizeof(g_displays[0])), g_display_count);

    g_displays[g_display_count].display_id = display_id;
    g_displays[g_display_count].bounds = bounds;
    g_displays[g_display_count].is_builtin = is_builtin;
    g_displays[g_display_count].vendor_number = vendor_number;
    g_displays[g_display_count].model_number = model_number;
    g_displays[g_display_count].serial_number = serial_number;
    g_displays[g_display_count].uuid = uuid;
    g_display_count++;
}

static const LockDockTestDisplay *lockdock_test_find_display(
    CGDirectDisplayID display_id) {
    for (uint32_t i = 0; i < g_display_count; i++) {
        if (g_displays[i].display_id == display_id) {
            return &g_displays[i];
        }
    }

    return NULL;
}

#include "../../src/lockdockd/lockdockd_display.c"

CGError CGGetActiveDisplayList(uint32_t max_displays,
                               CGDirectDisplayID *active_displays,
                               uint32_t *display_count) {
    uint32_t count = g_display_count;

    if (count > max_displays) {
        count = max_displays;
    }

    if (active_displays != NULL) {
        for (uint32_t i = 0; i < count; i++) {
            active_displays[i] = g_displays[i].display_id;
        }
    }

    if (display_count != NULL) {
        *display_count = count;
    }

    return kCGErrorSuccess;
}

boolean_t CGDisplayIsBuiltin(CGDirectDisplayID display_id) {
    const LockDockTestDisplay *display = lockdock_test_find_display(display_id);

    return display != NULL && display->is_builtin;
}

uint32_t CGDisplayVendorNumber(CGDirectDisplayID display_id) {
    const LockDockTestDisplay *display = lockdock_test_find_display(display_id);

    return display == NULL ? 0 : display->vendor_number;
}

uint32_t CGDisplayModelNumber(CGDirectDisplayID display_id) {
    const LockDockTestDisplay *display = lockdock_test_find_display(display_id);

    return display == NULL ? 0 : display->model_number;
}

uint32_t CGDisplaySerialNumber(CGDirectDisplayID display_id) {
    const LockDockTestDisplay *display = lockdock_test_find_display(display_id);

    return display == NULL ? 0 : display->serial_number;
}

CFUUIDRef CGDisplayCreateUUIDFromDisplayID(CGDirectDisplayID display_id) {
    const LockDockTestDisplay *display = lockdock_test_find_display(display_id);
    CFStringRef text;
    CFUUIDRef uuid;

    if (display == NULL || display->uuid == NULL || display->uuid[0] == '\0') {
        return NULL;
    }

    text = CFStringCreateWithCString(kCFAllocatorDefault, display->uuid,
                                     kCFStringEncodingUTF8);
    if (text == NULL) {
        return NULL;
    }

    uuid = CFUUIDCreateFromString(kCFAllocatorDefault, text);
    CFRelease(text);
    return uuid;
}

CGRect CGDisplayBounds(CGDirectDisplayID display_id) {
    const LockDockTestDisplay *display = lockdock_test_find_display(display_id);

    return display == NULL ? CGRectZero : display->bounds;
}

void setUp(void) {
    memset(g_displays, 0, sizeof(g_displays));
    g_display_count = 0;
}

void tearDown(void) {}

static void test_display_identity_is_valid_accepts_any_stable_identifier(void) {
    LockDockdDisplayIdentity identity = {0};

    TEST_ASSERT_FALSE(lockdockd_display_identity_is_valid(NULL));
    TEST_ASSERT_FALSE(lockdockd_display_identity_is_valid(&identity));

    identity.is_builtin = true;
    TEST_ASSERT_TRUE(lockdockd_display_identity_is_valid(&identity));

    memset(&identity, 0, sizeof(identity));
    identity.vendor_number = 11;
    TEST_ASSERT_TRUE(lockdockd_display_identity_is_valid(&identity));

    memset(&identity, 0, sizeof(identity));
    snprintf(identity.uuid, sizeof(identity.uuid),
             "11111111-1111-1111-1111-111111111111");
    TEST_ASSERT_TRUE(lockdockd_display_identity_is_valid(&identity));
}

static void test_copy_display_identity_populates_uuid_and_numeric_fields(void) {
    LockDockdDisplayIdentity identity;

    lockdock_test_add_display(100, CGRectMake(0, 0, 100, 100), true, 77, 88, 99,
                              "11111111-1111-1111-1111-111111111111");

    TEST_ASSERT_TRUE(lockdockd_copy_display_identity(100, &identity));
    TEST_ASSERT_TRUE(identity.is_builtin);
    TEST_ASSERT_EQUAL_UINT32(77, identity.vendor_number);
    TEST_ASSERT_EQUAL_UINT32(88, identity.model_number);
    TEST_ASSERT_EQUAL_UINT32(99, identity.serial_number);
    TEST_ASSERT_EQUAL_STRING("11111111-1111-1111-1111-111111111111", identity.uuid);
}

static void test_find_active_display_by_identity_prefers_uuid_match(void) {
    LockDockdDisplayIdentity identity = {0};
    CGDirectDisplayID display_id = 0;

    lockdock_test_add_display(10, CGRectMake(0, 0, 100, 100), false, 1, 2, 3,
                              "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA");
    lockdock_test_add_display(20, CGRectMake(100, 0, 100, 100), false, 1, 2, 3,
                              "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB");

    snprintf(identity.uuid, sizeof(identity.uuid),
             "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB");
    identity.vendor_number = 1;
    identity.model_number = 2;
    identity.serial_number = 3;

    TEST_ASSERT_TRUE(
        lockdockd_find_active_display_by_identity(&identity, &display_id));
    TEST_ASSERT_EQUAL_UINT32(20, display_id);
}

static void test_find_active_display_by_identity_falls_back_to_numeric_identity(
    void) {
    LockDockdDisplayIdentity identity = {0};
    CGDirectDisplayID display_id = 0;

    lockdock_test_add_display(30, CGRectMake(0, 0, 100, 100), true, 4, 5, 6,
                              "cccccccc-cccc-cccc-cccc-cccccccccccc");

    snprintf(identity.uuid, sizeof(identity.uuid),
             "dddddddd-dddd-dddd-dddd-dddddddddddd");
    identity.is_builtin = true;
    identity.vendor_number = 4;
    identity.model_number = 5;
    identity.serial_number = 6;

    TEST_ASSERT_TRUE(
        lockdockd_find_active_display_by_identity(&identity, &display_id));
    TEST_ASSERT_EQUAL_UINT32(30, display_id);
}

static void test_find_display_index_and_display_at_point_use_active_displays(void) {
    lockdock_test_add_display(40, CGRectMake(0, 0, 120, 100), false, 0, 0, 0, NULL);
    lockdock_test_add_display(50, CGRectMake(120, 0, 120, 100), false, 0, 0, 0,
                              NULL);

    TEST_ASSERT_EQUAL_INT(1, lockdockd_find_display_index(50));
    TEST_ASSERT_EQUAL_INT(-1, lockdockd_find_display_index(999));
    TEST_ASSERT_EQUAL_UINT32(40,
                             lockdockd_find_display_at_point(CGPointMake(10, 10)));
    TEST_ASSERT_EQUAL_UINT32(50,
                             lockdockd_find_display_at_point(CGPointMake(130, 10)));
    TEST_ASSERT_EQUAL_UINT32(0,
                             lockdockd_find_display_at_point(CGPointMake(400, 10)));
}

static void test_find_safe_edge_segment_bottom_skips_overlaps(void) {
    LockDockdSafeSegment segment;

    lockdock_test_add_display(60, CGRectMake(0, 0, 100, 100), false, 0, 0, 0, NULL);
    lockdock_test_add_display(61, CGRectMake(20, 100, 20, 30), false, 0, 0, 0, NULL);
    lockdock_test_add_display(62, CGRectMake(60, 100, 40, 30), false, 0, 0, 0, NULL);

    segment = lockdockd_find_safe_edge_segment(60, LOCKDOCKD_ORIENT_BOTTOM);
    TEST_ASSERT_EQUAL_INT(0, (int)segment.start);
    TEST_ASSERT_EQUAL_INT(20, (int)segment.end);
    TEST_ASSERT_EQUAL_INT(20, (int)segment.width);
    TEST_ASSERT_EQUAL_INT(10, (int)segment.center);
}

static void test_find_safe_edge_segment_left_uses_largest_gap(void) {
    LockDockdSafeSegment segment;

    lockdock_test_add_display(70, CGRectMake(200, 0, 100, 100), false, 0, 0, 0,
                              NULL);
    lockdock_test_add_display(71, CGRectMake(150, 20, 50, 20), false, 0, 0, 0, NULL);
    lockdock_test_add_display(72, CGRectMake(150, 70, 50, 30), false, 0, 0, 0, NULL);

    segment = lockdockd_find_safe_edge_segment(70, LOCKDOCKD_ORIENT_LEFT);
    TEST_ASSERT_EQUAL_INT(40, (int)segment.start);
    TEST_ASSERT_EQUAL_INT(70, (int)segment.end);
    TEST_ASSERT_EQUAL_INT(30, (int)segment.width);
    TEST_ASSERT_EQUAL_INT(55, (int)segment.center);
}

static void test_find_safe_edge_segment_right_merges_adjacent_overlaps(void) {
    LockDockdSafeSegment segment;

    lockdock_test_add_display(80, CGRectMake(400, 0, 100, 100), false, 0, 0, 0,
                              NULL);
    lockdock_test_add_display(81, CGRectMake(500, 0, 50, 25), false, 0, 0, 0, NULL);
    lockdock_test_add_display(82, CGRectMake(500, 25, 50, 25), false, 0, 0, 0, NULL);
    lockdock_test_add_display(83, CGRectMake(500, 80, 50, 20), false, 0, 0, 0, NULL);

    segment = lockdockd_find_safe_edge_segment(80, LOCKDOCKD_ORIENT_RIGHT);
    TEST_ASSERT_EQUAL_INT(50, (int)segment.start);
    TEST_ASSERT_EQUAL_INT(80, (int)segment.end);
    TEST_ASSERT_EQUAL_INT(30, (int)segment.width);
    TEST_ASSERT_EQUAL_INT(65, (int)segment.center);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_display_identity_is_valid_accepts_any_stable_identifier);
    RUN_TEST(test_copy_display_identity_populates_uuid_and_numeric_fields);
    RUN_TEST(test_find_active_display_by_identity_prefers_uuid_match);
    RUN_TEST(test_find_active_display_by_identity_falls_back_to_numeric_identity);
    RUN_TEST(test_find_display_index_and_display_at_point_use_active_displays);
    RUN_TEST(test_find_safe_edge_segment_bottom_skips_overlaps);
    RUN_TEST(test_find_safe_edge_segment_left_uses_largest_gap);
    RUN_TEST(test_find_safe_edge_segment_right_merges_adjacent_overlaps);
    return UNITY_END();
}
