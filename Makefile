VERSION = 0.2.0

SRC_DIR = src
BUILD_DIR = build
ARTIFACTS_DIR = $(BUILD_DIR)/artifacts
INSTALL_DIR = ${HOME}/.local/bin
THIRDPARTY_DIR = thirdparty
TARGET = $(BUILD_DIR)/lockdock

SRCS = \
	$(SRC_DIR)/main.c \
	$(SRC_DIR)/lockdock_daemon.c \
	$(SRC_DIR)/lockdock_display.c \
	$(SRC_DIR)/lockdock_locker.c \
	$(SRC_DIR)/lockdock_platform.c \
	$(SRC_DIR)/lockdock_preferences.c \
	$(SRC_DIR)/lockdock_runtime.c \
	$(SRC_DIR)/lockdock_launchagent.c \
	$(SRC_DIR)/lockdock_ipc.c
OBJS = $(patsubst $(SRC_DIR)/%.c,$(ARTIFACTS_DIR)/%.o,$(SRCS))

CC = clang
CLANG_TIDY ?= clang-tidy
OPTFLAGS ?= -O3 -flto
CFLAGS = \
	-std=c11 \
	-Wall -Wextra \
	-I$(THIRDPARTY_DIR) \
	-DLOCKDOCK_VERSION="\"$(VERSION)\""
FRAMEWORKS = \
	-framework CoreGraphics \
	-framework ApplicationServices \
	-framework ColorSync \
	-framework CoreFoundation

.PHONY: all clean fmt install publish tidy

all: $(TARGET)

$(ARTIFACTS_DIR):
	mkdir -p $(ARTIFACTS_DIR)

$(ARTIFACTS_DIR)/%.o: $(SRC_DIR)/%.c | $(ARTIFACTS_DIR)
	$(CC) $(CFLAGS) $(OPTFLAGS) -c -o $@ $<

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OPTFLAGS) -o $@ $(OBJS) $(FRAMEWORKS)

clean:
	rm -rf $(BUILD_DIR)

fmt:
	find src/ tests/ \
		\( -iname '*.h' -o -iname '*.c' \) \
		-not -path "*/thirdparty/*" \
		| xargs clang-format -i

tidy:
	$(CLANG_TIDY) -quiet $(SRCS) -- $(CFLAGS)

install: $(TARGET)
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

include tests/build.mk
