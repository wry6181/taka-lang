CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -Isrc -MMD -MP
LDFLAGS :=

SRC_DIR   := src
BUILD_DIR := build
TARGET    := $(BUILD_DIR)/taka

# Minden .c fájl rekurzívan a src/ alatt (pl. src/utils/*.c, src/lexer/*.c)
SRCS := $(shell find $(SRC_DIR) -name '*.c')
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# Alapértelmezett: release build
all: CFLAGS += -O2
all: $(TARGET)

# Debug build: make debug
debug: CFLAGS += -O0 -g3 -DDEBUG
debug: $(TARGET)

# Release build: make release
release: CFLAGS += -O2 -DNDEBUG
release: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Futtatás: make run [ARGS="..."]
run: all
	./$(TARGET) $(ARGS)

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean all

# compile_commands.json a clangd-nek (szerkesztő hibajelzéshez): make compdb
compdb:
	bear -- make -B

.PHONY: all debug release run clean rebuild compdb

-include $(DEPS)
