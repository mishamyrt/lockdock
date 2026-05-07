#include <Unity/unity.h>

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/lockdock_platform.c"

CGDirectDisplayID lockdock_find_display_at_point(CGPoint point) {
    (void)point;
    return 0;
}

static const LockDockDisplayNameEntry *lockdock_test_find_entry(
    const LockDockDisplayNameEntry *entries,
    size_t entry_count,
    CGDirectDisplayID display_id) {
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].display_id == display_id) {
            return &entries[i];
        }
    }

    return NULL;
}

static json_value_t *lockdock_test_parse_json_value(const char *text) {
    json_parse_result_t parse_result = {0};

    return json_parse_ex(text, strlen(text), json_parse_flags_default, NULL, NULL,
                         &parse_result);
}

static FILE *lockdock_test_open_stream(const char *text) {
    FILE *stream = tmpfile();

    TEST_ASSERT_NOT_NULL(stream);
    TEST_ASSERT_EQUAL_UINT64(strlen(text), fwrite(text, 1, strlen(text), stream));
    rewind(stream);
    return stream;
}

void setUp(void) {
    lockdock_invalidate_display_name_cache();
}

void tearDown(void) {
    lockdock_invalidate_display_name_cache();
}

static void test_parse_display_id_accepts_decimal_uint32_values(void) {
    CGDirectDisplayID display_id = 0;

    TEST_ASSERT_TRUE(lockdock_parse_display_id("1", &display_id));
    TEST_ASSERT_EQUAL_UINT32(1, display_id);

    TEST_ASSERT_TRUE(lockdock_parse_display_id("4294967295", &display_id));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, display_id);
}

static void test_parse_display_id_rejects_empty_zero_and_non_numeric_values(void) {
    CGDirectDisplayID display_id = 0;

    TEST_ASSERT_FALSE(lockdock_parse_display_id(NULL, &display_id));
    TEST_ASSERT_FALSE(lockdock_parse_display_id("", &display_id));
    TEST_ASSERT_FALSE(lockdock_parse_display_id("0", &display_id));
    TEST_ASSERT_FALSE(lockdock_parse_display_id("-1", &display_id));
    TEST_ASSERT_FALSE(lockdock_parse_display_id("abc", &display_id));
    TEST_ASSERT_FALSE(lockdock_parse_display_id("4294967296", &display_id));
}

static void test_parse_display_id_json_value_accepts_string_and_number_forms(void) {
    CGDirectDisplayID display_id = 0;
    json_value_t *value = lockdock_test_parse_json_value("\"12345\"");

    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_TRUE(lockdock_parse_display_id_json_value(value, &display_id));
    TEST_ASSERT_EQUAL_UINT32(12345, display_id);
    free(value);

    value = lockdock_test_parse_json_value("67890");
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_TRUE(lockdock_parse_display_id_json_value(value, &display_id));
    TEST_ASSERT_EQUAL_UINT32(67890, display_id);
    free(value);

    value = lockdock_test_parse_json_value("true");
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_FALSE(lockdock_parse_display_id_json_value(value, &display_id));
    free(value);
}

static void test_cache_display_name_updates_existing_entry(void) {
    LockDockDisplayNameEntry entries[lockdock_MAX_DISPLAYS] = {0};
    size_t entry_count = 0;

    lockdock_cache_display_name(entries, &entry_count, 42, "First");
    lockdock_cache_display_name(entries, &entry_count, 42, "Updated");
    lockdock_cache_display_name(entries, &entry_count, 77, "Second");

    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)entry_count);
    TEST_ASSERT_EQUAL_STRING("Updated", entries[0].name);
    TEST_ASSERT_EQUAL_STRING("Second", entries[1].name);
}

static void test_parse_system_profiler_stream_collects_display_names(void) {
    const char *json_text =
        "{"
        "  \"root\": {"
        "    \"list\": ["
        "      {"
        "        \"_name\": \"Studio Display\","
        "        \"_spdisplays_displayID\": \"111\""
        "      },"
        "      {"
        "        \"_name\": \"Projector\","
        "        \"_spdisplays_displayID\": \"222\","
        "        \"_spdisplays_CGSDID\": \"999\""
        "      },"
        "      {"
        "        \"_name\": \"Built-in\","
        "        \"_spdisplays_CGSDID\": 333"
        "      }"
        "    ]"
        "  }"
        "}";
    LockDockDisplayNameEntry entries[lockdock_MAX_DISPLAYS] = {0};
    size_t entry_count = 0;
    FILE *stream = lockdock_test_open_stream(json_text);
    const LockDockDisplayNameEntry *entry;

    TEST_ASSERT_TRUE(
        lockdock_parse_system_profiler_stream(stream, entries, &entry_count));
    fclose(stream);

    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)entry_count);

    entry = lockdock_test_find_entry(entries, entry_count, 111);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_STRING("Studio Display", entry->name);

    entry = lockdock_test_find_entry(entries, entry_count, 222);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_STRING("Projector", entry->name);

    entry = lockdock_test_find_entry(entries, entry_count, 333);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_STRING("Built-in", entry->name);
}

static void test_parse_system_profiler_stream_rejects_invalid_json(void) {
    LockDockDisplayNameEntry entries[lockdock_MAX_DISPLAYS] = {0};
    size_t entry_count = 0;
    FILE *stream = lockdock_test_open_stream("{");

    TEST_ASSERT_FALSE(
        lockdock_parse_system_profiler_stream(stream, entries, &entry_count));
    fclose(stream);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)entry_count);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_display_id_accepts_decimal_uint32_values);
    RUN_TEST(test_parse_display_id_rejects_empty_zero_and_non_numeric_values);
    RUN_TEST(test_parse_display_id_json_value_accepts_string_and_number_forms);
    RUN_TEST(test_cache_display_name_updates_existing_entry);
    RUN_TEST(test_parse_system_profiler_stream_collects_display_names);
    RUN_TEST(test_parse_system_profiler_stream_rejects_invalid_json);
    return UNITY_END();
}
