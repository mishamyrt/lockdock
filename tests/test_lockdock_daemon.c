#include <Unity/unity.h>

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../src/lockdock_display.h"
#include "../src/lockdock_ipc.h"
#include "../src/lockdock_runtime.h"

static CGDirectDisplayID g_locker_target = 0;
static bool g_preferred_display_saved = false;
static LockDockDisplayIdentity g_preferred_identity;
static CGDirectDisplayID g_preferred_display = 0;
static bool g_preferred_display_active = false;
static CGDirectDisplayID g_active_displays[LOCKDOCK_MAX_DISPLAYS];
static uint32_t g_active_display_count = 0;
static LockDockStatus g_status;
static int g_query_status_count = 0;
static int g_relocate_count = 0;
static CGDirectDisplayID g_relocated_display = 0;
static bool g_relocate_success = true;
static int g_set_target_count = 0;
static int g_clear_target_count = 0;
static int g_refresh_display_cache_count = 0;
static int g_usleep_count = 0;
static useconds_t g_last_usleep = 0;
static int g_settle_sleep_count = 0;
static int g_update_status_after_relocate_count = 0;
static int g_status_location_after_relocate = -1;

#include "../src/lockdock_daemon.c"

static bool lockdock_test_identity_equals(const LockDockDisplayIdentity *left,
                                          const LockDockDisplayIdentity *right) {
    return left != NULL && right != NULL && left->is_builtin == right->is_builtin &&
           left->vendor_number == right->vendor_number &&
           left->model_number == right->model_number &&
           left->serial_number == right->serial_number &&
           strcmp(left->uuid, right->uuid) == 0;
}

void lockdock_invalidate_display_name_cache(void) {}

void lockdock_invalidate_dock_orientation_cache(void) {}

int lockdock_find_display_index(CGDirectDisplayID display_id) {
    for (uint32_t i = 0; i < g_active_display_count; i++) {
        if (g_active_displays[i] == display_id) {
            return (int)i;
        }
    }

    return -1;
}

bool lockdock_find_active_display_by_identity(
    const LockDockDisplayIdentity *identity,
    CGDirectDisplayID *display_id_out) {
    if (!g_preferred_display_active ||
        !lockdock_test_identity_equals(identity, &g_preferred_identity)) {
        return false;
    }

    if (display_id_out != NULL) {
        *display_id_out = g_preferred_display;
    }

    return true;
}

bool lockdock_copy_display_identity(CGDirectDisplayID display_id,
                                    LockDockDisplayIdentity *identity_out) {
    if (display_id != g_preferred_display || identity_out == NULL) {
        return false;
    }

    *identity_out = g_preferred_identity;
    return true;
}

bool lockdock_copy_display_label(CGDirectDisplayID display_id,
                                 char *buffer,
                                 size_t buffer_size) {
    if (buffer == NULL || buffer_size == 0) {
        return false;
    }

    snprintf(buffer, buffer_size, "Display-%u", display_id);
    return true;
}

bool lockdock_preferences_load_preferred_display(
    LockDockDisplayIdentity *identity_out) {
    if (!g_preferred_display_saved) {
        return false;
    }

    if (identity_out != NULL) {
        *identity_out = g_preferred_identity;
    }

    return true;
}

bool lockdock_preferences_save_preferred_display(
    const LockDockDisplayIdentity *identity,
    char *error,
    size_t error_size) {
    (void)error;
    (void)error_size;

    if (identity == NULL) {
        return false;
    }

    g_preferred_identity = *identity;
    g_preferred_display_saved = true;
    return true;
}

bool lockdock_preferences_clear_preferred_display(char *error, size_t error_size) {
    (void)error;
    (void)error_size;

    g_preferred_display_saved = false;
    memset(&g_preferred_identity, 0, sizeof(g_preferred_identity));
    return true;
}

bool lockdock_locker_set_target(CGDirectDisplayID display_id,
                                char *error,
                                size_t error_size) {
    (void)error;
    (void)error_size;

    g_locker_target = display_id;
    g_set_target_count++;
    return true;
}

void lockdock_locker_refresh_display_cache(void) {
    g_refresh_display_cache_count++;
}

void lockdock_locker_clear_target(void) {
    g_locker_target = 0;
    g_clear_target_count++;
}

CGDirectDisplayID lockdock_locker_get_target(void) {
    return g_locker_target;
}

void lockdock_locker_shutdown(void) {
    lockdock_locker_clear_target();
}

bool lockdock_query_status(LockDockStatus *status, char *error, size_t error_size) {
    (void)error;
    (void)error_size;

    g_query_status_count++;
    if (status != NULL) {
        *status = g_status;
    }

    return true;
}

int lockdock_status_index_for_display(const LockDockStatus *status,
                                      CGDirectDisplayID display_id) {
    if (status == NULL) {
        return -1;
    }

    for (uint32_t i = 0; i < status->display_count; i++) {
        if (status->displays[i] == display_id) {
            return (int)i;
        }
    }

    return -1;
}

bool lockdock_relocate_display(CGDirectDisplayID display_id,
                               char *error,
                               size_t error_size) {
    g_relocate_count++;
    g_relocated_display = display_id;

    if (g_update_status_after_relocate_count > 0 &&
        g_relocate_count >= g_update_status_after_relocate_count) {
        g_status.location_index = g_status_location_after_relocate;
    }

    if (!g_relocate_success) {
        snprintf(error, error_size, "relocate failed");
        return false;
    }

    return true;
}

bool lockdock_ipc_ensure_socket_dir(char *error, size_t error_size) {
    (void)error;
    (void)error_size;
    return true;
}

bool lockdock_ipc_copy_socket_path(char *buffer,
                                   size_t buffer_size,
                                   char *error,
                                   size_t error_size) {
    (void)error;
    (void)error_size;
    snprintf(buffer, buffer_size, "/tmp/lockdock-test.sock");
    return true;
}

bool lockdock_ipc_copy_pid_path(char *buffer,
                                size_t buffer_size,
                                char *error,
                                size_t error_size) {
    (void)error;
    (void)error_size;
    snprintf(buffer, buffer_size, "/tmp/lockdock-test.pid");
    return true;
}

bool lockdock_ipc_parse_request_json(const char *request_json,
                                     LockDockIpcRequest *request_out,
                                     char *error,
                                     size_t error_size) {
    (void)request_json;
    (void)request_out;
    (void)error;
    (void)error_size;
    return false;
}

bool lockdock_ipc_serialize_state_response_json(const LockDockIpcState *state,
                                                char *buffer,
                                                size_t buffer_size,
                                                char *error,
                                                size_t error_size) {
    (void)state;
    (void)error;
    (void)error_size;
    snprintf(buffer, buffer_size, "{}");
    return true;
}

bool lockdock_ipc_serialize_result_response_json(const LockDockIpcResult *result,
                                                 char *buffer,
                                                 size_t buffer_size,
                                                 char *error,
                                                 size_t error_size) {
    (void)result;
    (void)error;
    (void)error_size;
    snprintf(buffer, buffer_size, "{}");
    return true;
}

int usleep(useconds_t usec) {
    g_usleep_count++;
    g_last_usleep = usec;
    if (usec == LOCKDOCK_DISPLAY_RECONFIGURATION_SETTLE_DELAY_US) {
        g_settle_sleep_count++;
    }
    return 0;
}

void setUp(void) {
    g_locker_target = 0;
    g_preferred_display_saved = false;
    memset(&g_preferred_identity, 0, sizeof(g_preferred_identity));
    g_preferred_display = 0;
    g_preferred_display_active = false;
    memset(g_active_displays, 0, sizeof(g_active_displays));
    g_active_display_count = 0;
    memset(&g_status, 0, sizeof(g_status));
    g_status.location_index = -1;
    g_query_status_count = 0;
    g_relocate_count = 0;
    g_relocated_display = 0;
    g_relocate_success = true;
    g_set_target_count = 0;
    g_clear_target_count = 0;
    g_refresh_display_cache_count = 0;
    g_usleep_count = 0;
    g_last_usleep = 0;
    g_settle_sleep_count = 0;
    g_update_status_after_relocate_count = 0;
    g_status_location_after_relocate = -1;
    atomic_store(&g_display_state_dirty, false);
}

void tearDown(void) {}

static void lockdock_test_save_preferred_display(CGDirectDisplayID display_id) {
    g_preferred_display_saved = true;
    g_preferred_display = display_id;
    g_preferred_identity.vendor_number = 11;
    g_preferred_identity.model_number = 22;
    g_preferred_identity.serial_number = 33;
    snprintf(g_preferred_identity.uuid, sizeof(g_preferred_identity.uuid),
             "11111111-1111-1111-1111-111111111111");
}

static void test_reconnect_relocates_dock_before_restoring_lock(void) {
    char error[LOCKDOCK_ERROR_BUFFER_SIZE] = "";

    lockdock_test_save_preferred_display(200);
    g_locker_target = 200;
    g_active_displays[0] = 100;
    g_active_display_count = 1;

    TEST_ASSERT_TRUE(lockdock_reconcile_display_state(error, sizeof(error)));
    TEST_ASSERT_EQUAL_UINT32(0, g_locker_target);
    TEST_ASSERT_EQUAL_INT(1, g_clear_target_count);
    TEST_ASSERT_EQUAL_INT(0, g_relocate_count);
    TEST_ASSERT_EQUAL_INT(0, g_set_target_count);

    g_preferred_display_active = true;
    g_active_displays[1] = 200;
    g_active_display_count = 2;
    g_status.display_count = 2;
    g_status.displays[0] = 100;
    g_status.displays[1] = 200;
    g_status.location_index = 1;

    TEST_ASSERT_TRUE(lockdock_reconcile_display_state(error, sizeof(error)));
    TEST_ASSERT_EQUAL_INT(1, g_query_status_count);
    TEST_ASSERT_EQUAL_INT(1, g_relocate_count);
    TEST_ASSERT_EQUAL_UINT32(200, g_relocated_display);
    TEST_ASSERT_EQUAL_INT(1, g_set_target_count);
    TEST_ASSERT_EQUAL_UINT32(200, g_locker_target);
}

static void test_reconnect_does_not_restore_lock_when_relocation_fails(void) {
    char error[LOCKDOCK_ERROR_BUFFER_SIZE] = "";

    lockdock_test_save_preferred_display(300);
    g_preferred_display_active = true;
    g_active_displays[0] = 100;
    g_active_displays[1] = 300;
    g_active_display_count = 2;
    g_relocate_success = false;

    TEST_ASSERT_FALSE(lockdock_reconcile_display_state(error, sizeof(error)));
    TEST_ASSERT_EQUAL_INT(1, g_relocate_count);
    TEST_ASSERT_EQUAL_UINT32(300, g_relocated_display);
    TEST_ASSERT_EQUAL_INT(0, g_set_target_count);
    TEST_ASSERT_EQUAL_UINT32(0, g_locker_target);
    TEST_ASSERT_EQUAL_STRING("relocate failed", error);
}

static void test_active_lock_relocates_dock_when_it_drifted(void) {
    char error[LOCKDOCK_ERROR_BUFFER_SIZE] = "";

    g_locker_target = 200;
    g_active_displays[0] = 100;
    g_active_displays[1] = 200;
    g_active_display_count = 2;
    g_status.display_count = 2;
    g_status.displays[0] = 100;
    g_status.displays[1] = 200;
    g_status.location_index = 0;
    g_update_status_after_relocate_count = 1;
    g_status_location_after_relocate = 1;

    TEST_ASSERT_TRUE(lockdock_reconcile_display_state(error, sizeof(error)));
    TEST_ASSERT_EQUAL_INT(2, g_query_status_count);
    TEST_ASSERT_EQUAL_INT(1, g_relocate_count);
    TEST_ASSERT_EQUAL_UINT32(200, g_relocated_display);
    TEST_ASSERT_EQUAL_INT(0, g_set_target_count);
    TEST_ASSERT_EQUAL_UINT32(200, g_locker_target);
}

static void test_reconnect_retries_when_first_relocation_probe_is_stale(void) {
    char error[LOCKDOCK_ERROR_BUFFER_SIZE] = "";

    lockdock_test_save_preferred_display(500);
    g_preferred_display_active = true;
    g_active_displays[0] = 100;
    g_active_displays[1] = 500;
    g_active_display_count = 2;
    g_status.display_count = 2;
    g_status.displays[0] = 100;
    g_status.displays[1] = 500;
    g_status.location_index = 0;
    g_update_status_after_relocate_count = 2;
    g_status_location_after_relocate = 1;

    TEST_ASSERT_TRUE(lockdock_reconcile_display_state(error, sizeof(error)));
    TEST_ASSERT_EQUAL_INT(2, g_relocate_count);
    TEST_ASSERT_EQUAL_INT(2, g_query_status_count);
    TEST_ASSERT_EQUAL_UINT32(500, g_relocated_display);
    TEST_ASSERT_EQUAL_UINT32(500, g_locker_target);
}

static void test_pending_display_reconcile_waits_for_reconfiguration_to_settle(
    void) {
    char error[LOCKDOCK_ERROR_BUFFER_SIZE] = "";

    lockdock_test_save_preferred_display(400);
    g_preferred_display_active = true;
    g_active_displays[0] = 400;
    g_active_display_count = 1;
    g_status.display_count = 1;
    g_status.displays[0] = 400;
    g_status.location_index = 0;
    atomic_store(&g_display_state_dirty, true);

    lockdock_reconcile_pending_display_state(error, sizeof(error));
    TEST_ASSERT_EQUAL_INT(1, g_settle_sleep_count);
    TEST_ASSERT_EQUAL_INT(1, g_relocate_count);
    TEST_ASSERT_EQUAL_UINT32(400, g_locker_target);
}

static void test_pending_display_reconcile_refreshes_cache_for_active_lock(void) {
    char error[LOCKDOCK_ERROR_BUFFER_SIZE] = "";

    g_locker_target = 600;
    g_active_displays[0] = 600;
    g_active_display_count = 1;
    g_status.display_count = 1;
    g_status.displays[0] = 600;
    g_status.location_index = 0;
    atomic_store(&g_display_state_dirty, true);

    lockdock_reconcile_pending_display_state(error, sizeof(error));
    TEST_ASSERT_EQUAL_INT(1, g_refresh_display_cache_count);
    TEST_ASSERT_EQUAL_UINT32(600, g_locker_target);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reconnect_relocates_dock_before_restoring_lock);
    RUN_TEST(test_reconnect_does_not_restore_lock_when_relocation_fails);
    RUN_TEST(test_active_lock_relocates_dock_when_it_drifted);
    RUN_TEST(test_reconnect_retries_when_first_relocation_probe_is_stale);
    RUN_TEST(test_pending_display_reconcile_waits_for_reconfiguration_to_settle);
    RUN_TEST(test_pending_display_reconcile_refreshes_cache_for_active_lock);
    return UNITY_END();
}
