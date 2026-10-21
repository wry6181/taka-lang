CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -Isrc -MMD -MP
LDFLAGS := -lpthread

SRC_DIR   := src
BUILD_DIR := build
TARGET    := $(BUILD_DIR)/taka

# Hash map backend: khash (default, battle-tested klib) | arena (native)
# Swap: make MAP_BACKEND=arena   — only utils/map_<backend>.c changes.
MAP_BACKEND ?= khash

# Minden .c fájl rekurzívan a src/ alatt (pl. src/utils/*.c, src/lexer/*.c),
# a map backend-ek kivéve: abból csak az aktuális MAP_BACKEND fordul be.
ALL_SRCS := $(shell find $(SRC_DIR) -name '*.c')
SRCS := $(filter-out $(SRC_DIR)/utils/map_%.c, $(ALL_SRCS))
SRCS += $(SRC_DIR)/utils/map_$(MAP_BACKEND).c
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

# ---- map backend tesztek (mindkét backend ugyanazt adja) ------------------
TEST_DIR   := tests
TEST_BUILD := $(BUILD_DIR)/tests
TEST_CFLAGS := -std=c11 -Wall -Wextra -Werror -I$(SRC_DIR) -g -O1 \
               -fsanitize=address,undefined -fno-sanitize-recover=all
TEST_LDFLAGS := -lpthread
BENCH_CFLAGS := -std=c11 -Wall -Wextra -Werror -I$(SRC_DIR) -O2

# make test: konformitás (a két backend kimenete byte-egyezik) + robustness,
# ASan+UBSan alatt, mindkét backenden. A vector egyetlen implementáció.
test:
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_DIR)/map_conform.c $(SRC_DIR)/utils/map_khash.c $(SRC_DIR)/utils/arena.c $(TEST_CFLAGS) -o $(TEST_BUILD)/conform_khash
	$(CC) $(TEST_DIR)/map_conform.c $(SRC_DIR)/utils/map_arena.c $(SRC_DIR)/utils/arena.c $(TEST_CFLAGS) -o $(TEST_BUILD)/conform_arena
	$(CC) $(TEST_DIR)/map_robust.c $(SRC_DIR)/utils/map_khash.c $(SRC_DIR)/utils/arena.c $(TEST_CFLAGS) -o $(TEST_BUILD)/robust_khash
	$(CC) $(TEST_DIR)/map_robust.c $(SRC_DIR)/utils/map_arena.c $(SRC_DIR)/utils/arena.c $(TEST_CFLAGS) -o $(TEST_BUILD)/robust_arena
	$(CC) $(TEST_DIR)/vector_test.c $(SRC_DIR)/utils/vector.c $(SRC_DIR)/utils/arena.c $(TEST_CFLAGS) -o $(TEST_BUILD)/vector_test
	$(CC) $(TEST_DIR)/parser_test.c $(SRC_DIR)/parser/parser.c $(SRC_DIR)/lexer/tokens.c \
	    $(SRC_DIR)/utils/map_khash.c $(SRC_DIR)/utils/vector.c $(SRC_DIR)/utils/arena.c \
	    $(SRC_DIR)/utils/string.c $(SRC_DIR)/utils/logger.c $(TEST_CFLAGS) -o $(TEST_BUILD)/parser_test $(TEST_LDFLAGS)
	$(CC) $(TEST_DIR)/expr_test.c $(SRC_DIR)/parser/parser.c $(SRC_DIR)/lexer/tokens.c \
	    $(SRC_DIR)/utils/map_khash.c $(SRC_DIR)/utils/vector.c $(SRC_DIR)/utils/arena.c \
	    $(SRC_DIR)/utils/string.c $(SRC_DIR)/utils/logger.c $(TEST_CFLAGS) -o $(TEST_BUILD)/expr_test $(TEST_LDFLAGS)
	$(TEST_BUILD)/conform_khash > $(TEST_BUILD)/conform_khash.out
	$(TEST_BUILD)/conform_arena > $(TEST_BUILD)/conform_arena.out
	@diff $(TEST_BUILD)/conform_khash.out $(TEST_BUILD)/conform_arena.out
	@echo "conform: both backends produced identical results:"
	@cat $(TEST_BUILD)/conform_khash.out
	$(TEST_BUILD)/robust_khash khash
	$(TEST_BUILD)/robust_arena arena
	$(TEST_BUILD)/vector_test
	$(TEST_BUILD)/parser_test
	$(TEST_BUILD)/expr_test
	@echo "map+vector+parser+expr tests OK"

# make bench: sebesség-mérés -O2-ben, sanitizerek NÉLKÜL (ASan torzítana).
bench:
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_DIR)/map_bench.c $(SRC_DIR)/utils/map_khash.c $(SRC_DIR)/utils/arena.c $(BENCH_CFLAGS) -o $(TEST_BUILD)/bench_khash
	$(CC) $(TEST_DIR)/map_bench.c $(SRC_DIR)/utils/map_arena.c $(SRC_DIR)/utils/arena.c $(BENCH_CFLAGS) -o $(TEST_BUILD)/bench_arena
	$(TEST_BUILD)/bench_khash khash
	@echo
	$(TEST_BUILD)/bench_arena arena

.PHONY: all debug release run clean rebuild compdb test bench

-include $(DEPS)
