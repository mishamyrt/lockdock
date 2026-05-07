#include <Unity/unity.h>

#include "../src/lockdock_preferences.h"
#include "test_support.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char g_temp_home[PATH_MAX];
static char g_preferences_domain[128];
static LockDockTestEnvGuard g_home_guard;
static LockDockTestEnvGuard g_domain_guard;

static void lockdock_test_assert_identity_equal(
    const LockDockDisplayIdentity *expected,
    const LockDockDisplayIdentity *actual) {
    TEST_ASSERT_EQUAL_INT(expected->is_builtin, actual->is_builtin);
    TEST_ASSERT_EQUAL_UINT32(expected->vendor_number, actual->vendor_number);
    TEST_ASSERT_EQUAL_UINT32(expected->model_number, actual->model_number);
    TEST_ASSERT_EQUAL_UINT32(expected->serial_number, actual->serial_number);
    TEST_ASSERT_EQUAL_STRING(expected->uuid, actual->uuid);
}

void setUp(void) {
    char error[512];

    memset(g_temp_home, 0, sizeof(g_temp_home));
    memset(g_preferences_domain, 0, sizeof(g_preferences_domain));
    memset(&g_home_guard, 0, sizeof(g_home_guard));
    memset(&g_domain_guard, 0, sizeof(g_domain_guard));

    TEST_ASSERT_TRUE(lockdock_test_make_temp_dir(g_temp_home, sizeof(g_temp_home)));
    TEST_ASSERT_TRUE(lockdock_test_push_env(&g_home_guard, "HOME", g_temp_home));

    snprintf(g_preferences_domain, sizeof(g_preferences_domain),
             "co.myrt.lockdock.tests.%u", (unsigned)getpid());
    TEST_ASSERT_TRUE(lockdock_test_push_env(
        &g_domain_guard, "LOCKDOCK_TEST_PREFERENCES_DOMAIN", g_preferences_domain));

    if (!lockdock_preferences_clear_preferred_display(error, sizeof(error))) {
        fprintf(stderr, "setUp clear failed: %s\n", error);
        TEST_FAIL_MESSAGE(error);
    }
}

void tearDown(void) {
    char error[512];

    if (!lockdock_preferences_clear_preferred_display(error, sizeof(error))) {
        fprintf(stderr, "tearDown clear failed: %s\n", error);
        TEST_FAIL_MESSAGE(error);
    }
    lockdock_test_pop_env(&g_domain_guard);
    lockdock_test_pop_env(&g_home_guard);

    if (g_temp_home[0] != '\0') {
        TEST_ASSERT_TRUE(lockdock_test_remove_tree(g_temp_home));
    }
}

static void test_preferences_save_load_and_clear_uuid_backed_display(void) {
    LockDockDisplayIdentity expected = {0};
    LockDockDisplayIdentity actual = {0};
    char error[512];

    expected.vendor_number = 1;
    expected.model_number = 2;
    expected.serial_number = 3;
    snprintf(expected.uuid, sizeof(expected.uuid),
             "11111111-1111-1111-1111-111111111111");

    TEST_ASSERT_TRUE(lockdock_preferences_save_preferred_display(&expected, error,
                                                                 sizeof(error)));
    TEST_ASSERT_TRUE(lockdock_preferences_load_preferred_display(&actual));
    lockdock_test_assert_identity_equal(&expected, &actual);

    TEST_ASSERT_TRUE(
        lockdock_preferences_clear_preferred_display(error, sizeof(error)));
    TEST_ASSERT_FALSE(lockdock_preferences_load_preferred_display(&actual));
}

static void test_preferences_round_trip_builtin_only_identity(void) {
    LockDockDisplayIdentity expected = {0};
    LockDockDisplayIdentity actual = {0};
    char error[512];

    expected.is_builtin = true;

    TEST_ASSERT_TRUE(lockdock_preferences_save_preferred_display(&expected, error,
                                                                 sizeof(error)));
    TEST_ASSERT_TRUE(lockdock_preferences_load_preferred_display(&actual));
    lockdock_test_assert_identity_equal(&expected, &actual);
}

static void test_preferences_reject_invalid_identity(void) {
    LockDockDisplayIdentity invalid_identity = {0};
    char error[512];

    TEST_ASSERT_FALSE(lockdock_preferences_save_preferred_display(
        &invalid_identity, error, sizeof(error)));
    TEST_ASSERT_EQUAL_STRING("Internal error", error);
}

static void test_preferences_clear_leaves_empty_state(void) {
    LockDockDisplayIdentity identity = {0};
    LockDockDisplayIdentity actual = {0};
    char error[512];

    identity.vendor_number = 10;
    identity.model_number = 20;

    TEST_ASSERT_TRUE(lockdock_preferences_save_preferred_display(&identity, error,
                                                                 sizeof(error)));
    TEST_ASSERT_TRUE(lockdock_preferences_load_preferred_display(&actual));

    TEST_ASSERT_TRUE(
        lockdock_preferences_clear_preferred_display(error, sizeof(error)));
    memset(&actual, 0, sizeof(actual));
    TEST_ASSERT_FALSE(lockdock_preferences_load_preferred_display(&actual));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_preferences_save_load_and_clear_uuid_backed_display);
    RUN_TEST(test_preferences_round_trip_builtin_only_identity);
    RUN_TEST(test_preferences_reject_invalid_identity);
    RUN_TEST(test_preferences_clear_leaves_empty_state);
    return UNITY_END();
}
