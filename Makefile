VERSION = 0.1.0

SRC_DIR = src
BUILD_DIR = build
INSTALL_DIR = ${HOME}/.local/bin

DAEMON_SRC_DIR = $(SRC_DIR)/lockdockd
DAEMON_BUILD_DIR = $(BUILD_DIR)/artifacts/lockdockd
DAEMON_TARGET = $(BUILD_DIR)/lockdockd
DAEMON_SRCS = \
	$(DAEMON_SRC_DIR)/main.c \
	$(DAEMON_SRC_DIR)/lockdockd_daemon.c \
	$(DAEMON_SRC_DIR)/lockdockd_display.c \
	$(DAEMON_SRC_DIR)/lockdockd_locker.c \
	$(DAEMON_SRC_DIR)/lockdockd_platform.c \
	$(DAEMON_SRC_DIR)/lockdockd_preferences.c \
	$(DAEMON_SRC_DIR)/lockdockd_runtime.c
DAEMON_OBJS = $(patsubst $(DAEMON_SRC_DIR)/%.c,$(DAEMON_BUILD_DIR)/%.o,$(DAEMON_SRCS))

CLI_SRC_DIR = $(SRC_DIR)/lockdock_cli
CLI_BUILD_DIR = $(BUILD_DIR)/artifacts/lockdock
CLI_TARGET = $(BUILD_DIR)/lockdock
CLI_SRCS = \
	$(CLI_SRC_DIR)/main.c \
	$(CLI_SRC_DIR)/lockdock_launchagent.c
CLI_OBJS = $(patsubst $(CLI_SRC_DIR)/%.c,$(CLI_BUILD_DIR)/%.o,$(CLI_SRCS))

IPC_SRC_DIR = $(SRC_DIR)/lockdock_ipc
IPC_BUILD_DIR = $(BUILD_DIR)/artifacts/lockdock_ipc
IPC_SRCS = \
	$(IPC_SRC_DIR)/lockdock_ipc.c
IPC_OBJS = $(patsubst $(IPC_SRC_DIR)/%.c,$(IPC_BUILD_DIR)/%.o,$(IPC_SRCS))

OPTFLAGS ?= -O3 -flto
CFLAGS = \
	-std=c11 \
	-Wall -Wextra \
	-I$(IPC_SRC_DIR) \
	-Isrc \
	-DAPP_VERSION="\"$(VERSION)\"" \
	$(OPTFLAGS)
FRAMEWORKS = \
	-framework CoreGraphics \
	-framework ApplicationServices \
	-framework ColorSync \
	-framework CoreFoundation

.PHONY: all clean install publish

all: $(DAEMON_TARGET) $(CLI_TARGET)

$(IPC_BUILD_DIR)/%.o: $(IPC_SRC_DIR)/%.c | $(IPC_BUILD_DIR)
	clang $(CFLAGS) -c -o $@ $<
$(IPC_BUILD_DIR):
	mkdir -p $(IPC_BUILD_DIR)

$(DAEMON_TARGET): $(IPC_OBJS) $(DAEMON_OBJS)
	clang $(CFLAGS) -o $@ $(IPC_OBJS) $(DAEMON_OBJS) $(FRAMEWORKS)
$(DAEMON_BUILD_DIR)/%.o: $(DAEMON_SRC_DIR)/%.c | $(DAEMON_BUILD_DIR)
	clang $(CFLAGS) -c -o $@ $<
$(DAEMON_BUILD_DIR):
	mkdir -p $(DAEMON_BUILD_DIR)

$(CLI_TARGET): $(IPC_OBJS) $(CLI_OBJS)
	clang $(CFLAGS) -o $@ $(IPC_OBJS) $(CLI_OBJS) $(FRAMEWORKS)
$(CLI_BUILD_DIR)/%.o: $(CLI_SRC_DIR)/%.c | $(CLI_BUILD_DIR)
	clang $(CFLAGS) -c -o $@ $<
$(CLI_BUILD_DIR):
	mkdir -p $(CLI_BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

fmt:
	find src/ \
		\( -iname '*.h' -o -iname '*.c' \) \
		-not -path "*/thirdparty/*" \
		| xargs clang-format -i

install: $(CLI_TARGET) $(DAEMON_TARGET)
	mkdir -p $(INSTALL_DIR)
	cp $^ $(INSTALL_DIR)

publish:
	git add Makefile
	git commit -m "chore: release ${VERSION} 🔥"
	git tag "v${VERSION}"
	git-cliff -o CHANGELOG.md
	git tag -d "v${VERSION}"
	git add CHANGELOG.md
	git commit --amend --no-edit
	git tag -a "v${VERSION}" -m "release v${VERSION}"
	git push
	git push --tags
