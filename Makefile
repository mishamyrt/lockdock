VERSION = 0.1.0
TARGET = lockdockd
SRC_DIR = src
OBJS = \
	$(SRC_DIR)/main.o \
	$(SRC_DIR)/lockdockd_commands.o \
	$(SRC_DIR)/lockdockd_daemon.o \
	$(SRC_DIR)/lockdockd_display.o \
	$(SRC_DIR)/lockdockd_ipc.o \
	$(SRC_DIR)/lockdockd_locker.o \
	$(SRC_DIR)/lockdockd_platform.o \
	$(SRC_DIR)/lockdockd_runtime.o

OPTFLAGS ?= -O3 -march=native -flto
CFLAGS = -std=c11 -Wall -Wextra -DAPP_VERSION="\"$(VERSION)\"" $(OPTFLAGS)
FRAMEWORKS = \
	-framework CoreGraphics \
	-framework ApplicationServices \
	-framework CoreFoundation

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	clang $(FRAMEWORKS) -o $@ $(OBJS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	clang $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

fmt:
	find src/ \
		-iname '*.h' \
		-o -iname '*.c' \
		| xargs clang-format -i
