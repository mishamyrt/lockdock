TEST_DIR = tests
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

IPC_TEST_TARGET = $(TEST_BUILD_DIR)/lockdock_ipc_tests
LAUNCHAGENT_TEST_TARGET = $(TEST_BUILD_DIR)/lockdock_launchagent_tests
DISPLAY_TEST_TARGET = $(TEST_BUILD_DIR)/lockdock_display_tests
DAEMON_TEST_TARGET = $(TEST_BUILD_DIR)/lockdock_daemon_tests
RUNTIME_TEST_TARGET = $(TEST_BUILD_DIR)/lockdock_runtime_tests
PLATFORM_TEST_TARGET = $(TEST_BUILD_DIR)/lockdock_platform_tests
PREFERENCES_TEST_TARGET = $(TEST_BUILD_DIR)/lockdock_preferences_tests
TEST_UNIT_TARGETS = \
	$(IPC_TEST_TARGET) \
	$(LAUNCHAGENT_TEST_TARGET) \
	$(DISPLAY_TEST_TARGET) \
	$(DAEMON_TEST_TARGET) \
	$(RUNTIME_TEST_TARGET) \
	$(PLATFORM_TEST_TARGET) \
	$(PREFERENCES_TEST_TARGET)

$(TEST_BUILD_DIR):
	mkdir -p $(TEST_BUILD_DIR)

$(IPC_TEST_TARGET): \
	$(TEST_DIR)/test_lockdock_ipc.c \
	$(TEST_DIR)/test_support.c \
	$(UNITY_DIR)/unity.c | $(TEST_BUILD_DIR)
	clang $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

$(LAUNCHAGENT_TEST_TARGET): \
	$(TEST_DIR)/test_lockdock_launchagent.c \
	$(TEST_DIR)/test_support.c \
	$(UNITY_DIR)/unity.c | $(TEST_BUILD_DIR)
	clang $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

$(DISPLAY_TEST_TARGET): \
	$(TEST_DIR)/test_lockdock_display.c \
	$(UNITY_DIR)/unity.c | $(TEST_BUILD_DIR)
	clang $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

$(DAEMON_TEST_TARGET): \
	$(TEST_DIR)/test_lockdock_daemon.c \
	$(UNITY_DIR)/unity.c | $(TEST_BUILD_DIR)
	clang $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

$(RUNTIME_TEST_TARGET): \
	$(TEST_DIR)/test_lockdock_runtime.c \
	$(UNITY_DIR)/unity.c | $(TEST_BUILD_DIR)
	clang $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

$(PLATFORM_TEST_TARGET): \
	$(TEST_DIR)/test_lockdock_platform.c \
	$(UNITY_DIR)/unity.c | $(TEST_BUILD_DIR)
	clang $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

$(PREFERENCES_TEST_TARGET): \
	$(TEST_DIR)/test_lockdock_preferences.c \
	$(TEST_DIR)/test_support.c \
	$(UNITY_DIR)/unity.c \
	$(SRC_DIR)/lockdock_preferences.c \
	$(SRC_DIR)/lockdock_display.c | $(TEST_BUILD_DIR)
	clang $(TEST_CFLAGS) -DLOCKDOCK_TESTING -o $@ $^ $(TEST_LDFLAGS)

.PHONY: test test-unit test-smoke

test-unit: $(TEST_UNIT_TARGETS)
	$(IPC_TEST_TARGET)
	$(LAUNCHAGENT_TEST_TARGET)
	$(DISPLAY_TEST_TARGET)
	$(DAEMON_TEST_TARGET)
	$(RUNTIME_TEST_TARGET)
	$(PLATFORM_TEST_TARGET)
	$(PREFERENCES_TEST_TARGET)

test-smoke: all
	sh $(TEST_DIR)/smoke.sh $(VERSION)

test: all test-unit test-smoke
