#include <Unity/unity.h>

#include "../src/lockdock_ipc.h"

#include "test_support.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static char g_temp_home[PATH_MAX];
static LockDockTestEnvGuard g_home_guard;

typedef struct {
    int fake_fd;
    bool socket_called;
    bool connect_called;
    bool shutdown_called;
    bool close_called;
    char connected_path[PATH_MAX];
    char request[LOCKDOCK_IPC_MAX_MESSAGE];
    size_t request_used;
    char response[LOCKDOCK_IPC_MAX_MESSAGE];
    size_t response_offset;
} LockDockTestTransport;

static LockDockTestTransport g_transport;

static int lockdock_test_socket(int domain, int type, int protocol);
static int lockdock_test_connect(int fd,
                                 const struct sockaddr *address,
                                 socklen_t address_length);
static ssize_t lockdock_test_write(int fd, const void *buffer, size_t count);
static ssize_t lockdock_test_read(int fd, void *buffer, size_t count);
static int lockdock_test_shutdown(int fd, int how);
static int lockdock_test_close(int fd);

#define socket lockdock_test_socket
#define connect lockdock_test_connect
#define write lockdock_test_write
#define read lockdock_test_read
#define shutdown lockdock_test_shutdown
#define close lockdock_test_close
#include "../src/lockdock_ipc.c"
#undef socket
#undef connect
#undef write
#undef read
#undef shutdown
#undef close

static void lockdock_test_assert_contains(const char *text, const char *needle) {
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_NOT_NULL(needle);
    TEST_ASSERT_NOT_NULL(strstr(text, needle));
}

static void lockdock_test_prepare_transport(const char *response) {
    memset(&g_transport, 0, sizeof(g_transport));
    g_transport.fake_fd = 123;

    if (response != NULL) {
        snprintf(g_transport.response, sizeof(g_transport.response), "%s", response);
    }
}

static int lockdock_test_socket(int domain, int type, int protocol) {
    TEST_ASSERT_EQUAL_INT(AF_UNIX, domain);
    TEST_ASSERT_EQUAL_INT(SOCK_STREAM, type);
    TEST_ASSERT_EQUAL_INT(0, protocol);

    g_transport.socket_called = true;
    return g_transport.fake_fd;
}

static int lockdock_test_connect(int fd,
                                 const struct sockaddr *address,
                                 socklen_t address_length) {
    const struct sockaddr_un *unix_address = (const struct sockaddr_un *)address;

    TEST_ASSERT_EQUAL_INT(g_transport.fake_fd, fd);
    TEST_ASSERT_TRUE(address_length >= sizeof(sa_family_t));
    TEST_ASSERT_NOT_NULL(address);
    TEST_ASSERT_EQUAL_INT(AF_UNIX, unix_address->sun_family);

    g_transport.connect_called = true;
    snprintf(g_transport.connected_path, sizeof(g_transport.connected_path), "%s",
             unix_address->sun_path);
    return 0;
}

static ssize_t lockdock_test_write(int fd, const void *buffer, size_t count) {
    TEST_ASSERT_EQUAL_INT(g_transport.fake_fd, fd);
    TEST_ASSERT_TRUE(g_transport.request_used + count < sizeof(g_transport.request));
    TEST_ASSERT_NOT_NULL(buffer);

    memcpy(g_transport.request + g_transport.request_used, buffer, count);
    g_transport.request_used += count;
    g_transport.request[g_transport.request_used] = '\0';
    return (ssize_t)count;
}

static ssize_t lockdock_test_read(int fd, void *buffer, size_t count) {
    size_t remaining;
    size_t copy_size;

    TEST_ASSERT_EQUAL_INT(g_transport.fake_fd, fd);
    TEST_ASSERT_NOT_NULL(buffer);

    remaining = strlen(g_transport.response) - g_transport.response_offset;
    if (remaining == 0) {
        return 0;
    }

    copy_size = remaining;
    if (copy_size > count) {
        copy_size = count;
    }

    memcpy(buffer, g_transport.response + g_transport.response_offset, copy_size);
    g_transport.response_offset += copy_size;
    return (ssize_t)copy_size;
}

static int lockdock_test_shutdown(int fd, int how) {
    TEST_ASSERT_EQUAL_INT(g_transport.fake_fd, fd);
    TEST_ASSERT_EQUAL_INT(SHUT_WR, how);

    g_transport.shutdown_called = true;
    return 0;
}

static int lockdock_test_close(int fd) {
    TEST_ASSERT_EQUAL_INT(g_transport.fake_fd, fd);

    g_transport.close_called = true;
    return 0;
}

static void lockdock_test_assert_transport_request(const char *expected_request) {
    char expected_path[PATH_MAX];
    char error[LOCKDOCK_IPC_REASON_SIZE];

    TEST_ASSERT_TRUE(g_transport.socket_called);
    TEST_ASSERT_TRUE(g_transport.connect_called);
    TEST_ASSERT_TRUE(g_transport.shutdown_called);
    TEST_ASSERT_TRUE(g_transport.close_called);
    TEST_ASSERT_EQUAL_STRING(expected_request, g_transport.request);

    TEST_ASSERT_TRUE(lockdock_ipc_copy_socket_path(
        expected_path, sizeof(expected_path), error, sizeof(error)));
    TEST_ASSERT_EQUAL_STRING(expected_path, g_transport.connected_path);
}

static void lockdock_test_build_many_displays_response(char *buffer,
                                                       size_t buffer_size) {
    size_t used = 0;

    TEST_ASSERT_NOT_NULL(buffer);
    TEST_ASSERT_GREATER_THAN_UINT32(0, (uint32_t)buffer_size);

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"displays\":[");
    for (uint32_t i = 0; i <= LOCKDOCK_IPC_MAX_DISPLAYS; i++) {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "%s\"D%u\"",
                                 i == 0 ? "" : ",", i);
    }
    snprintf(buffer + used, buffer_size - used, "],\"location\":0}");
}

void setUp(void) {
    memset(g_temp_home, 0, sizeof(g_temp_home));
    memset(&g_home_guard, 0, sizeof(g_home_guard));
    memset(&g_transport, 0, sizeof(g_transport));

    TEST_ASSERT_TRUE(lockdock_test_make_temp_dir(g_temp_home, sizeof(g_temp_home)));
    TEST_ASSERT_TRUE(lockdock_test_push_env(&g_home_guard, "HOME", g_temp_home));
}

void tearDown(void) {
    lockdock_test_pop_env(&g_home_guard);

    if (g_temp_home[0] != '\0') {
        TEST_ASSERT_TRUE(lockdock_test_remove_tree(g_temp_home));
    }
}

static void test_parse_request_json_accepts_supported_commands(void) {
    LockDockIpcRequest request;
    char error[LOCKDOCK_IPC_REASON_SIZE];

    TEST_ASSERT_TRUE(lockdock_ipc_parse_request_json(
        "{\"cmd\":\"get_state\"}", &request, error, sizeof(error)));
    TEST_ASSERT_EQUAL_INT(LOCKDOCK_IPC_COMMAND_GET_STATE, request.command);
    TEST_ASSERT_FALSE(request.has_target);

    TEST_ASSERT_TRUE(lockdock_ipc_parse_request_json(
        "{\"cmd\":\"unlock\"}", &request, error, sizeof(error)));
    TEST_ASSERT_EQUAL_INT(LOCKDOCK_IPC_COMMAND_UNLOCK, request.command);
    TEST_ASSERT_FALSE(request.has_target);

    TEST_ASSERT_TRUE(lockdock_ipc_parse_request_json(
        "{\"cmd\":\"set_state\",\"target\":7}", &request, error, sizeof(error)));
    TEST_ASSERT_EQUAL_INT(LOCKDOCK_IPC_COMMAND_SET_STATE, request.command);
    TEST_ASSERT_TRUE(request.has_target);
    TEST_ASSERT_EQUAL_INT(7, request.target);
}

static void test_parse_request_json_rejects_missing_duplicate_and_unknown_fields(
    void) {
    LockDockIpcRequest request;
    char error[LOCKDOCK_IPC_REASON_SIZE];

    TEST_ASSERT_FALSE(
        lockdock_ipc_parse_request_json("{}", &request, error, sizeof(error)));
    lockdock_test_assert_contains(error, "Missing required field 'cmd'");

    TEST_ASSERT_FALSE(
        lockdock_ipc_parse_request_json("{\"cmd\":\"get_state\",\"cmd\":\"unlock\"}",
                                        &request, error, sizeof(error)));
    lockdock_test_assert_contains(error, "Field 'cmd' must be specified once");

    TEST_ASSERT_FALSE(lockdock_ipc_parse_request_json(
        "{\"cmd\":\"set_state\",\"target\":1,\"target\":2}", &request, error,
        sizeof(error)));
    lockdock_test_assert_contains(error, "Field 'target' must be specified once");

    TEST_ASSERT_FALSE(lockdock_ipc_parse_request_json(
        "{\"cmd\":\"get_state\",\"extra\":1}", &request, error, sizeof(error)));
    lockdock_test_assert_contains(error, "Unknown field 'extra'");
}

static void test_parse_request_json_rejects_invalid_json_and_target_values(void) {
    LockDockIpcRequest request;
    char error[LOCKDOCK_IPC_REASON_SIZE];

    TEST_ASSERT_FALSE(
        lockdock_ipc_parse_request_json("{", &request, error, sizeof(error)));
    lockdock_test_assert_contains(error, "Invalid JSON request");

    TEST_ASSERT_FALSE(lockdock_ipc_parse_request_json(
        "{\"cmd\":\"set_state\",\"target\":\"1\"}", &request, error, sizeof(error)));
    lockdock_test_assert_contains(error, "Field 'target' must be an integer");

    TEST_ASSERT_FALSE(lockdock_ipc_parse_request_json(
        "{\"cmd\":\"set_state\",\"target\":-1}", &request, error, sizeof(error)));
    lockdock_test_assert_contains(error,
                                  "Field 'target' must be a non-negative integer");

    TEST_ASSERT_FALSE(lockdock_ipc_parse_request_json(
        "{\"cmd\":\"set_state\",\"target\":2147483648}", &request, error,
        sizeof(error)));
    lockdock_test_assert_contains(error,
                                  "Field 'target' must be a non-negative integer");
}

static void test_serialize_request_json_emits_expected_payloads(void) {
    LockDockIpcRequest request = {0};
    char buffer[LOCKDOCK_IPC_MAX_MESSAGE];
    char error[LOCKDOCK_IPC_REASON_SIZE];

    request.command = LOCKDOCK_IPC_COMMAND_GET_STATE;
    TEST_ASSERT_TRUE(lockdock_ipc_serialize_request_json(
        &request, buffer, sizeof(buffer), error, sizeof(error)));
    TEST_ASSERT_EQUAL_STRING("{\"cmd\":\"get_state\"}", buffer);

    request.command = LOCKDOCK_IPC_COMMAND_SET_STATE;
    request.has_target = true;
    request.target = 3;
    TEST_ASSERT_TRUE(lockdock_ipc_serialize_request_json(
        &request, buffer, sizeof(buffer), error, sizeof(error)));
    TEST_ASSERT_EQUAL_STRING("{\"cmd\":\"set_state\",\"target\":3}", buffer);
}

static void test_parse_response_json_accepts_state_and_result_payloads(void) {
    LockDockIpcResponse response;
    char error[LOCKDOCK_IPC_REASON_SIZE];

    TEST_ASSERT_TRUE(lockdock_ipc_parse_response_json(
        "{\"displays\":[\"A\",\"B\"],\"location\":1,\"target\":0}", &response, error,
        sizeof(error)));
    TEST_ASSERT_EQUAL_INT(LOCKDOCK_IPC_RESPONSE_STATE, response.kind);
    TEST_ASSERT_EQUAL_UINT32(2, response.state.display_count);
    TEST_ASSERT_EQUAL_STRING("A", response.state.displays[0]);
    TEST_ASSERT_EQUAL_STRING("B", response.state.displays[1]);
    TEST_ASSERT_EQUAL_INT(1, response.state.location_index);
    TEST_ASSERT_TRUE(response.state.has_target);
    TEST_ASSERT_EQUAL_INT(0, response.state.target_index);

    TEST_ASSERT_TRUE(
        lockdock_ipc_parse_response_json("{\"success\":false,\"reason\":\"Denied\"}",
                                         &response, error, sizeof(error)));
    TEST_ASSERT_EQUAL_INT(LOCKDOCK_IPC_RESPONSE_RESULT, response.kind);
    TEST_ASSERT_FALSE(response.result.success);
    TEST_ASSERT_EQUAL_STRING("Denied", response.result.reason);
}

static void test_parse_response_json_rejects_invalid_shapes(void) {
    LockDockIpcResponse response;
    char error[LOCKDOCK_IPC_REASON_SIZE];
    char buffer[LOCKDOCK_IPC_MAX_MESSAGE];

    TEST_ASSERT_FALSE(lockdock_ipc_parse_response_json(
        "{\"displays\":[],\"location\":0,\"success\":true}", &response, error,
        sizeof(error)));
    lockdock_test_assert_contains(error, "Response mixes result and state fields");

    TEST_ASSERT_FALSE(lockdock_ipc_parse_response_json(
        "{\"displays\":[\"A\"]}", &response, error, sizeof(error)));
    lockdock_test_assert_contains(error, "Missing required field 'location'");

    TEST_ASSERT_FALSE(lockdock_ipc_parse_response_json(
        "{\"displays\":\"A\",\"location\":0}", &response, error, sizeof(error)));
    lockdock_test_assert_contains(error,
                                  "Field 'displays' must be an array of strings");

    TEST_ASSERT_FALSE(lockdock_ipc_parse_response_json(
        "{\"success\":\"yes\"}", &response, error, sizeof(error)));
    lockdock_test_assert_contains(error, "Field 'success' must be a boolean");

    lockdock_test_build_many_displays_response(buffer, sizeof(buffer));
    TEST_ASSERT_FALSE(
        lockdock_ipc_parse_response_json(buffer, &response, error, sizeof(error)));
    lockdock_test_assert_contains(error, "Response contains too many displays");
}

static void test_serialize_response_json_escapes_strings(void) {
    LockDockIpcState state = {0};
    LockDockIpcResult result = {0};
    char buffer[LOCKDOCK_IPC_MAX_MESSAGE];
    char error[LOCKDOCK_IPC_REASON_SIZE];

    snprintf(state.displays[0], sizeof(state.displays[0]), "A \"quoted\" \\ tab\t");
    state.display_count = 1;
    state.location_index = 0;
    state.has_target = true;
    state.target_index = 0;

    TEST_ASSERT_TRUE(lockdock_ipc_serialize_state_response_json(
        &state, buffer, sizeof(buffer), error, sizeof(error)));
    lockdock_test_assert_contains(buffer, "\\\"quoted\\\"");
    lockdock_test_assert_contains(buffer, "\\\\");
    lockdock_test_assert_contains(buffer, "\\t");

    result.success = false;
    snprintf(result.reason, sizeof(result.reason), "Line 1\nLine 2");
    TEST_ASSERT_TRUE(lockdock_ipc_serialize_result_response_json(
        &result, buffer, sizeof(buffer), error, sizeof(error)));
    TEST_ASSERT_EQUAL_STRING("{\"success\":false,\"reason\":\"Line 1\\nLine 2\"}",
                             buffer);
}

static void test_socket_helpers_use_temp_home_directory(void) {
    char socket_dir[PATH_MAX];
    char socket_path[PATH_MAX];
    char expected_path[PATH_MAX];
    char error[LOCKDOCK_IPC_REASON_SIZE];

    TEST_ASSERT_TRUE(lockdock_ipc_ensure_socket_dir(error, sizeof(error)));
    TEST_ASSERT_TRUE(lockdock_ipc_copy_socket_path(socket_path, sizeof(socket_path),
                                                   error, sizeof(error)));
    TEST_ASSERT_TRUE(lockdock_test_join_path(expected_path, sizeof(expected_path),
                                             g_temp_home,
                                             "Library/Caches/co.myrt.lockdock/"
                                             "control.sock"));
    TEST_ASSERT_EQUAL_STRING(expected_path, socket_path);
    TEST_ASSERT_TRUE(lockdock_test_join_path(socket_dir, sizeof(socket_dir),
                                             g_temp_home,
                                             "Library/Caches/co.myrt.lockdock"));
    TEST_ASSERT_EQUAL_INT(0, access(socket_dir, F_OK));
}

static void test_socket_path_rejects_unix_domain_socket_overflow(void) {
    LockDockTestEnvGuard guard = {0};
    char long_home[PATH_MAX];
    char socket_path[PATH_MAX];
    char error[LOCKDOCK_IPC_REASON_SIZE];

    memset(long_home, 'a', sizeof(long_home));
    snprintf(long_home, sizeof(long_home),
             "/Users/example/with/a/very/very/very/very/very/very/very/very/"
             "very/long/home/path/for/lockdock/tests");

    TEST_ASSERT_TRUE(lockdock_test_push_env(&guard, "HOME", long_home));
    TEST_ASSERT_FALSE(lockdock_ipc_copy_socket_path(socket_path, sizeof(socket_path),
                                                    error, sizeof(error)));
    lockdock_test_assert_contains(error, "Unix domain socket");
    lockdock_test_pop_env(&guard);
}

static void test_client_ipc_get_state_round_trip_uses_socket_transport(void) {
    LockDockIpcState state;
    char error[LOCKDOCK_IPC_REASON_SIZE];

    lockdock_test_prepare_transport(
        "{\"displays\":[\"Left\",\"Right\"],\"location\":1,\"target\":0}");

    TEST_ASSERT_TRUE(lockdock_ipc_get_state(&state, error, sizeof(error)));
    lockdock_test_assert_transport_request("{\"cmd\":\"get_state\"}");
    TEST_ASSERT_EQUAL_UINT32(2, state.display_count);
    TEST_ASSERT_EQUAL_STRING("Left", state.displays[0]);
    TEST_ASSERT_EQUAL_STRING("Right", state.displays[1]);
    TEST_ASSERT_EQUAL_INT(1, state.location_index);
    TEST_ASSERT_TRUE(state.has_target);
    TEST_ASSERT_EQUAL_INT(0, state.target_index);
}

static void test_client_ipc_lock_and_unlock_round_trip_uses_socket_transport(void) {
    char error[LOCKDOCK_IPC_REASON_SIZE];

    lockdock_test_prepare_transport("{\"success\":true}");
    TEST_ASSERT_TRUE(lockdock_ipc_lock(2, error, sizeof(error)));
    lockdock_test_assert_transport_request("{\"cmd\":\"set_state\",\"target\":2}");

    lockdock_test_prepare_transport("{\"success\":true}");
    TEST_ASSERT_TRUE(lockdock_ipc_unlock(error, sizeof(error)));
    lockdock_test_assert_transport_request("{\"cmd\":\"unlock\"}");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_request_json_accepts_supported_commands);
    RUN_TEST(test_parse_request_json_rejects_missing_duplicate_and_unknown_fields);
    RUN_TEST(test_parse_request_json_rejects_invalid_json_and_target_values);
    RUN_TEST(test_serialize_request_json_emits_expected_payloads);
    RUN_TEST(test_parse_response_json_accepts_state_and_result_payloads);
    RUN_TEST(test_parse_response_json_rejects_invalid_shapes);
    RUN_TEST(test_serialize_response_json_escapes_strings);
    RUN_TEST(test_socket_helpers_use_temp_home_directory);
    RUN_TEST(test_socket_path_rejects_unix_domain_socket_overflow);
    RUN_TEST(test_client_ipc_get_state_round_trip_uses_socket_transport);
    RUN_TEST(test_client_ipc_lock_and_unlock_round_trip_uses_socket_transport);
    return UNITY_END();
}
