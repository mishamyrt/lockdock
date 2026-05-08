#include <Unity/unity.h>

#include "test_support.h"

#include "../src/lockdock_config.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/lockdock_launchagent.c"

static char g_temp_home[PATH_MAX];
static LockDockTestEnvGuard g_home_guard;

static void lockdock_test_assert_contains(const char *text, const char *needle) {
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_NOT_NULL(needle);
    TEST_ASSERT_NOT_NULL(strstr(text, needle));
}

void setUp(void) {
    memset(g_temp_home, 0, sizeof(g_temp_home));
    memset(&g_home_guard, 0, sizeof(g_home_guard));

    TEST_ASSERT_TRUE(lockdock_test_make_temp_dir(g_temp_home, sizeof(g_temp_home)));
    TEST_ASSERT_TRUE(lockdock_test_push_env(&g_home_guard, "HOME", g_temp_home));
}

void tearDown(void) {
    lockdock_test_pop_env(&g_home_guard);

    if (g_temp_home[0] != '\0') {
        TEST_ASSERT_TRUE(lockdock_test_remove_tree(g_temp_home));
    }
}

static void test_copy_launchagents_dir_uses_home_directory(void) {
    char path[PATH_MAX];
    char expected[PATH_MAX];
    char error[LOCKDOCK_LAUNCHAGENT_MESSAGE_SIZE];

    TEST_ASSERT_TRUE(
        lockdock_copy_launchagents_dir(path, sizeof(path), error, sizeof(error)));
    TEST_ASSERT_TRUE(lockdock_test_join_path(expected, sizeof(expected), g_temp_home,
                                             "Library/LaunchAgents"));
    TEST_ASSERT_EQUAL_STRING(expected, path);
}

static void test_copy_plist_path_uses_home_directory(void) {
    char path[PATH_MAX];
    char expected[PATH_MAX];
    char error[LOCKDOCK_LAUNCHAGENT_MESSAGE_SIZE];

    TEST_ASSERT_TRUE(
        lockdock_copy_plist_path(path, sizeof(path), error, sizeof(error)));
    TEST_ASSERT_TRUE(
        lockdock_test_join_path(expected, sizeof(expected), g_temp_home,
                                "Library/LaunchAgents/co.myrt.lockdock.plist"));
    TEST_ASSERT_EQUAL_STRING(expected, path);
}

static void test_build_plist_escapes_xml_special_characters(void) {
    char plist[4096];
    char error[LOCKDOCK_LAUNCHAGENT_MESSAGE_SIZE];

    TEST_ASSERT_TRUE(lockdock_build_plist(
        plist, sizeof(plist), "/tmp/A&B<'\"/lockdock", error, sizeof(error)));
    lockdock_test_assert_contains(plist, "<string>co.myrt.lockdock</string>");
    lockdock_test_assert_contains(plist, "/tmp/A&amp;B&lt;&apos;&quot;/lockdock");
    lockdock_test_assert_contains(plist, "<key>RunAtLoad</key>");
}

static void test_mkdir_p_creates_nested_directories(void) {
    char path[PATH_MAX];
    char error[LOCKDOCK_LAUNCHAGENT_MESSAGE_SIZE];

    TEST_ASSERT_TRUE(lockdock_test_join_path(path, sizeof(path), g_temp_home,
                                             "Library/LaunchAgents/Nested/Dir"));
    TEST_ASSERT_TRUE(lockdock_mkdir_p(path, error, sizeof(error)));
    TEST_ASSERT_EQUAL_INT(0, access(path, F_OK));
}

static void test_copy_launchctl_targets_use_current_uid(void) {
    char domain[64];
    char service[96];
    char error[LOCKDOCK_LAUNCHAGENT_MESSAGE_SIZE];
    char expected_domain[64];
    char expected_service[96];

    TEST_ASSERT_TRUE(
        lockdock_copy_domain_target(domain, sizeof(domain), error, sizeof(error)));
    TEST_ASSERT_TRUE(lockdock_copy_service_target(service, sizeof(service), error,
                                                  sizeof(error)));

    snprintf(expected_domain, sizeof(expected_domain), "gui/%u", (unsigned)getuid());
    snprintf(expected_service, sizeof(expected_service), "gui/%u/%s",
             (unsigned)getuid(), LOCKDOCK_BUNDLE_ID);

    TEST_ASSERT_EQUAL_STRING(expected_domain, domain);
    TEST_ASSERT_EQUAL_STRING(expected_service, service);
}

static void test_trim_trailing_whitespace_removes_line_endings_and_tabs(void) {
    char text[] = "hello \n\t\r";

    lockdock_trim_trailing_whitespace(text);
    TEST_ASSERT_EQUAL_STRING("hello", text);
}

static void test_run_command_captures_trimmed_output(void) {
    const char *const argv[] = {"/usr/bin/printf", "hello\n\t ", NULL};
    char output[LOCKDOCK_LAUNCHAGENT_MESSAGE_SIZE];
    char error[LOCKDOCK_LAUNCHAGENT_MESSAGE_SIZE];
    int exit_status = -1;

    TEST_ASSERT_TRUE(lockdock_run_command(argv, &exit_status, output, sizeof(output),
                                          error, sizeof(error)));
    TEST_ASSERT_EQUAL_INT(0, exit_status);
    TEST_ASSERT_EQUAL_STRING("hello", output);
}

static void test_run_command_returns_exec_failure_status(void) {
    const char *const argv[] = {"/definitely/missing/lockdock-command", NULL};
    char output[LOCKDOCK_LAUNCHAGENT_MESSAGE_SIZE];
    char error[LOCKDOCK_LAUNCHAGENT_MESSAGE_SIZE];
    int exit_status = -1;

    TEST_ASSERT_TRUE(lockdock_run_command(argv, &exit_status, output, sizeof(output),
                                          error, sizeof(error)));
    TEST_ASSERT_EQUAL_INT(127, exit_status);
    lockdock_test_assert_contains(output, "Failed to exec");
}

static void test_write_plist_writes_complete_file(void) {
    char directory[PATH_MAX];
    char path[PATH_MAX];
    char content[256];
    char file_text[512];
    char error[LOCKDOCK_LAUNCHAGENT_MESSAGE_SIZE];

    TEST_ASSERT_TRUE(lockdock_test_join_path(directory, sizeof(directory),
                                             g_temp_home, "Library/LaunchAgents"));
    TEST_ASSERT_TRUE(lockdock_test_join_path(path, sizeof(path), directory,
                                             "co.myrt.lockdock.plist"));

    TEST_ASSERT_TRUE(lockdock_mkdir_p(directory, error, sizeof(error)));
    snprintf(content, sizeof(content), "<plist>test</plist>\n");
    TEST_ASSERT_TRUE(lockdock_write_plist(path, content, error, sizeof(error)));
    TEST_ASSERT_TRUE(lockdock_test_read_file(path, file_text, sizeof(file_text)));
    TEST_ASSERT_EQUAL_STRING(content, file_text);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_copy_launchagents_dir_uses_home_directory);
    RUN_TEST(test_copy_plist_path_uses_home_directory);
    RUN_TEST(test_build_plist_escapes_xml_special_characters);
    RUN_TEST(test_mkdir_p_creates_nested_directories);
    RUN_TEST(test_copy_launchctl_targets_use_current_uid);
    RUN_TEST(test_trim_trailing_whitespace_removes_line_endings_and_tabs);
    RUN_TEST(test_run_command_captures_trimmed_output);
    RUN_TEST(test_run_command_returns_exec_failure_status);
    RUN_TEST(test_write_plist_writes_complete_file);
    return UNITY_END();
}
