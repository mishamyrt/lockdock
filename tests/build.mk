TEST_DIR = tests
TEST_SUPPORT_DIR = $(TEST_DIR)/support
TEST_BUILD_DIR = $(BUILD_DIR)/tests
UNITY_DIR = $(THIRDPARTY_DIR)/Unity

TEST_CFLAGS = \
	$(CFLAGS) \
	-I$(TEST_DIR) \
	-O0 \
	-Wno-unused-function \
	-Wno-unused-parameter
TEST_LDFLAGS = \
	$(FRAMEWORKS) \
	-pthread

IPC_TEST_TARGET = $(TEST_BUILD_DIR)/ipc_tests
CLI_TEST_TARGET = $(TEST_BUILD_DIR)/cli_tests
DISPLAY_TEST_TARGET = $(TEST_BUILD_DIR)/lockdockd_display_tests
RUNTIME_TEST_TARGET = $(TEST_BUILD_DIR)/lockdockd_runtime_tests
PLATFORM_TEST_TARGET = $(TEST_BUILD_DIR)/lockdockd_platform_tests
PREFERENCES_TEST_TARGET = $(TEST_BUILD_DIR)/lockdockd_preferences_tests
TEST_UNIT_TARGETS = \
	$(IPC_TEST_TARGET) \
	$(CLI_TEST_TARGET) \
	$(DISPLAY_TEST_TARGET) \
	$(RUNTIME_TEST_TARGET) \
	$(PLATFORM_TEST_TARGET) \
	$(PREFERENCES_TEST_TARGET)

$(TEST_BUILD_DIR):
	mkdir -p $(TEST_BUILD_DIR)

$(IPC_TEST_TARGET): \
	$(TEST_DIR)/ipc/test_lockdock_ipc.c \
	$(TEST_SUPPORT_DIR)/test_support.c \
	$(UNITY_DIR)/unity.c | $(TEST_BUILD_DIR)
	clang $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

$(CLI_TEST_TARGET): \
	$(TEST_DIR)/cli/test_lockdock_cli.c \
	$(TEST_SUPPORT_DIR)/test_support.c \
	$(UNITY_DIR)/unity.c | $(TEST_BUILD_DIR)
	clang $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

$(DISPLAY_TEST_TARGET): \
	$(TEST_DIR)/lockdockd/test_lockdockd_display.c \
	$(UNITY_DIR)/unity.c | $(TEST_BUILD_DIR)
	clang $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

$(RUNTIME_TEST_TARGET): \
	$(TEST_DIR)/lockdockd/test_lockdockd_runtime.c \
	$(UNITY_DIR)/unity.c | $(TEST_BUILD_DIR)
	clang $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

$(PLATFORM_TEST_TARGET): \
	$(TEST_DIR)/lockdockd/test_lockdockd_platform.c \
	$(UNITY_DIR)/unity.c | $(TEST_BUILD_DIR)
	clang $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

$(PREFERENCES_TEST_TARGET): \
	$(TEST_DIR)/lockdockd/test_lockdockd_preferences.c \
	$(TEST_SUPPORT_DIR)/test_support.c \
	$(UNITY_DIR)/unity.c \
	$(DAEMON_SRC_DIR)/lockdockd_preferences.c \
	$(DAEMON_SRC_DIR)/lockdockd_display.c | $(TEST_BUILD_DIR)
	clang $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

.PHONY: test test-unit test-smoke

test-unit: $(TEST_UNIT_TARGETS)
	$(IPC_TEST_TARGET)
	$(CLI_TEST_TARGET)
	$(DISPLAY_TEST_TARGET)
	$(RUNTIME_TEST_TARGET)
	$(PLATFORM_TEST_TARGET)
	$(PREFERENCES_TEST_TARGET)

test-smoke: all
	sh $(TEST_DIR)/test_smoke.sh $(VERSION)

test: all test-unit test-smoke
