VERSION = 0.1.0

SRC_DIR = src
BUILD_DIR = build

DAEMON_SRC_DIR = $(SRC_DIR)/lockdockd
DAEMON_BUILD_DIR = $(BUILD_DIR)/artifacts/lockdockd
DAEMON_TARGET = $(BUILD_DIR)/lockdockd
DAEMON_SRCS = \
	$(DAEMON_SRC_DIR)/main.c \
	$(DAEMON_SRC_DIR)/lockdockd_daemon.c \
	$(DAEMON_SRC_DIR)/lockdockd_display.c \
	$(DAEMON_SRC_DIR)/lockdockd_ipc.c \
	$(DAEMON_SRC_DIR)/lockdockd_launchagent.c \
	$(DAEMON_SRC_DIR)/lockdockd_locker.c \
	$(DAEMON_SRC_DIR)/lockdockd_platform.c \
	$(DAEMON_SRC_DIR)/lockdockd_preferences.c \
	$(DAEMON_SRC_DIR)/lockdockd_request.c \
	$(DAEMON_SRC_DIR)/lockdockd_runtime.c
DAEMON_OBJS = $(patsubst $(DAEMON_SRC_DIR)/%.c,$(DAEMON_BUILD_DIR)/%.o,$(DAEMON_SRCS))

OPTFLAGS ?= -O3 -march=native -flto
CFLAGS = -std=c11 -Wall -Wextra -DAPP_VERSION="\"$(VERSION)\"" $(OPTFLAGS)
FRAMEWORKS = \
	-framework CoreGraphics \
	-framework ApplicationServices \
	-framework ColorSync \
	-framework CoreFoundation

.PHONY: all clean

all: $(DAEMON_TARGET)

$(DAEMON_TARGET): $(DAEMON_OBJS)
	clang $(OPTFLAGS) -o $@ $(DAEMON_OBJS) $(FRAMEWORKS)

$(DAEMON_BUILD_DIR)/%.o: $(DAEMON_SRC_DIR)/%.c | $(DAEMON_BUILD_DIR)
	clang $(CFLAGS) -c -o $@ $<

$(DAEMON_BUILD_DIR):
	mkdir -p $(DAEMON_BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

fmt:
	find src/ \
		\( -iname '*.h' -o -iname '*.c' \) \
		-not -path "*/thirdparty/*" \
		| xargs clang-format -i
