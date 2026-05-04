TARGET = lockdockd
SRC_DIR = src
OBJS = \
	$(SRC_DIR)/main.o \
	$(SRC_DIR)/lockdockd_commands.o \
	$(SRC_DIR)/lockdockd_display.o \
	$(SRC_DIR)/lockdockd_objc_bridge.o

CFLAGS = -std=c11 -Wall -Wextra
OBJCFLAGS = -fobjc-arc
FRAMEWORKS = \
	-framework AppKit \
	-framework CoreGraphics \
	-framework Foundation \
	-framework IOKit \
	-framework ApplicationServices

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	clang $(FRAMEWORKS) -o $@ $(OBJS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	clang $(CFLAGS) -c -o $@ $<

$(SRC_DIR)/%.o: $(SRC_DIR)/%.m
	clang $(OBJCFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

fmt:
	find src/ \
		-iname '*.h' \
		-o -iname '*.m' \
		-o -iname '*.c' \
		| xargs clang-format -i
