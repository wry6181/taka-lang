CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -Isrc -MMD -MP
LDFLAGS := -lpthread

SRC_DIR   := src
BUILD_DIR := build
TARGET    := $(BUILD_DIR)/taka
DIAG_DEF  := $(SRC_DIR)/diagnostics/diagnostics.def
DIAG_MSG  := $(SRC_DIR)/diagnostics/diagnostics_messages.h

# Hash map backend: khash (default, battle-tested klib) | arena (native)
# Swap: make MAP_BACKEND=arena   — only utils/map_<backend>.c changes.
MAP_BACKEND ?= khash

# Test sanitizer set. Override with: make test SANITIZE=address,undefined
SANITIZE ?= undefined

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

$(DIAG_MSG): $(DIAG_DEF) tools/gen_diagnostics.py
	python3 tools/gen_diagnostics.py $(DIAG_DEF) $(DIAG_MSG)

$(BUILD_DIR)/diagnostics/diagnostics.o $(BUILD_DIR)/parser/parser.o \
    $(BUILD_DIR)/sema/sema.o $(BUILD_DIR)/codegen/codegen.o $(BUILD_DIR)/main.o: $(DIAG_MSG)

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
TEST_CFLAGS := -std=c11 -Wall -Wextra -Werror -I$(SRC_DIR) -g -O1
ifneq ($(SANITIZE),)
TEST_CFLAGS += -fsanitize=$(SANITIZE) -fno-sanitize-recover=all
endif
TEST_LDFLAGS := -lpthread
BENCH_CFLAGS := -std=c11 -Wall -Wextra -Werror -I$(SRC_DIR) -O2

# make test: konformitás (a két backend kimenete byte-egyezik) + robustness,
# UBSan alatt, mindkét backenden. ASan: make test SANITIZE=address,undefined
test: $(TARGET) $(DIAG_MSG)
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_DIR)/map_conform.c $(SRC_DIR)/utils/map_khash.c $(SRC_DIR)/utils/arena.c $(TEST_CFLAGS) -o $(TEST_BUILD)/conform_khash
	$(CC) $(TEST_DIR)/map_conform.c $(SRC_DIR)/utils/map_arena.c $(SRC_DIR)/utils/arena.c $(TEST_CFLAGS) -o $(TEST_BUILD)/conform_arena
	$(CC) $(TEST_DIR)/map_robust.c $(SRC_DIR)/utils/map_khash.c $(SRC_DIR)/utils/arena.c $(TEST_CFLAGS) -o $(TEST_BUILD)/robust_khash
	$(CC) $(TEST_DIR)/map_robust.c $(SRC_DIR)/utils/map_arena.c $(SRC_DIR)/utils/arena.c $(TEST_CFLAGS) -o $(TEST_BUILD)/robust_arena
	$(CC) $(TEST_DIR)/vector_test.c $(SRC_DIR)/utils/vector.c $(SRC_DIR)/utils/arena.c $(TEST_CFLAGS) -o $(TEST_BUILD)/vector_test
	$(CC) $(TEST_DIR)/parser_test.c $(SRC_DIR)/parser/parser.c $(SRC_DIR)/lexer/tokens.c \
	    $(SRC_DIR)/diagnostics/diagnostics.c $(SRC_DIR)/utils/map_khash.c $(SRC_DIR)/utils/vector.c $(SRC_DIR)/utils/arena.c \
	    $(SRC_DIR)/utils/string.c $(SRC_DIR)/utils/logger.c $(TEST_CFLAGS) -o $(TEST_BUILD)/parser_test $(TEST_LDFLAGS)
	$(CC) $(TEST_DIR)/expr_test.c $(SRC_DIR)/parser/parser.c $(SRC_DIR)/lexer/tokens.c \
	    $(SRC_DIR)/diagnostics/diagnostics.c $(SRC_DIR)/utils/map_khash.c $(SRC_DIR)/utils/vector.c $(SRC_DIR)/utils/arena.c \
	    $(SRC_DIR)/utils/string.c $(SRC_DIR)/utils/logger.c $(TEST_CFLAGS) -o $(TEST_BUILD)/expr_test $(TEST_LDFLAGS)
	$(CC) $(TEST_DIR)/diagnostics_test.c $(SRC_DIR)/diagnostics/diagnostics.c \
	    $(SRC_DIR)/utils/arena.c $(TEST_CFLAGS) -o $(TEST_BUILD)/diagnostics_test
	$(CC) $(TEST_DIR)/grammar_test.c $(SRC_DIR)/parser/parser.c $(SRC_DIR)/lexer/tokens.c \
	    $(SRC_DIR)/lexer/tokenizer.c $(SRC_DIR)/diagnostics/diagnostics.c \
	    $(SRC_DIR)/utils/map_khash.c $(SRC_DIR)/utils/vector.c $(SRC_DIR)/utils/arena.c \
	    $(SRC_DIR)/utils/string.c $(SRC_DIR)/utils/logger.c $(TEST_CFLAGS) -o $(TEST_BUILD)/grammar_test $(TEST_LDFLAGS)
	$(CC) $(TEST_DIR)/sema_test.c $(SRC_DIR)/parser/parser.c $(SRC_DIR)/lexer/tokens.c \
	    $(SRC_DIR)/lexer/tokenizer.c $(SRC_DIR)/sema/sema.c $(SRC_DIR)/diagnostics/diagnostics.c \
	    $(SRC_DIR)/utils/map_khash.c $(SRC_DIR)/utils/vector.c $(SRC_DIR)/utils/arena.c \
	    $(SRC_DIR)/utils/string.c $(SRC_DIR)/utils/logger.c $(TEST_CFLAGS) -o $(TEST_BUILD)/sema_test $(TEST_LDFLAGS)
	$(CC) $(TEST_DIR)/codegen_test.c $(SRC_DIR)/parser/parser.c $(SRC_DIR)/lexer/tokens.c \
	    $(SRC_DIR)/lexer/tokenizer.c $(SRC_DIR)/sema/sema.c $(SRC_DIR)/codegen/codegen.c \
	    $(SRC_DIR)/diagnostics/diagnostics.c $(SRC_DIR)/utils/map_khash.c $(SRC_DIR)/utils/vector.c $(SRC_DIR)/utils/arena.c \
	    $(SRC_DIR)/utils/string.c $(SRC_DIR)/utils/logger.c $(TEST_CFLAGS) -o $(TEST_BUILD)/codegen_test $(TEST_LDFLAGS)
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
	$(TEST_BUILD)/diagnostics_test
	$(TEST_BUILD)/grammar_test
	$(TEST_BUILD)/sema_test
	TAKA_TEST_SANITIZE="$(SANITIZE)" $(TEST_BUILD)/codegen_test
	python3 $(TEST_DIR)/lsp_test.py tools/taka_lsp.py $(TARGET)
	@for file in example/*.tk; do ./$(TARGET) "$$file" > /dev/null || exit 1; done
	@echo "map+vector+parser+expr+grammar+sema+codegen+lsp+example tests OK"

# make fuzz: libFuzzer + ASan/UBSan (clang szükséges).
FUZZ_CC ?= $(shell if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then echo /opt/homebrew/opt/llvm/bin/clang; else echo clang; fi)
fuzz: $(DIAG_MSG)
	@mkdir -p $(TEST_BUILD)/fuzz_corpus
	@cp example/*.tk $(TEST_BUILD)/fuzz_corpus/
	$(FUZZ_CC) -std=c11 -Wall -Wextra -Werror -I$(SRC_DIR) -g -O1 \
	    -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
	    $(TEST_DIR)/fuzz_parser.c $(SRC_DIR)/parser/parser.c $(SRC_DIR)/lexer/tokens.c \
	    $(SRC_DIR)/lexer/tokenizer.c $(SRC_DIR)/diagnostics/diagnostics.c $(SRC_DIR)/utils/map_arena.c $(SRC_DIR)/utils/vector.c \
	    $(SRC_DIR)/utils/arena.c $(SRC_DIR)/utils/string.c $(SRC_DIR)/utils/logger.c \
	    -o $(TEST_BUILD)/fuzz_parser $(TEST_LDFLAGS)
	ASAN_OPTIONS=detect_leaks=0 ./$(TEST_BUILD)/fuzz_parser -max_total_time=60 -timeout=5 \
	    $(TEST_BUILD)/fuzz_corpus

test-asan:
	$(MAKE) clean
	$(MAKE) CC=$(FUZZ_CC) test SANITIZE=address,undefined

install-zed:
	./tools/install_zed_extension.sh

analyze:
	scan-build --status-bugs $(MAKE) CC=$(FUZZ_CC) clean all

# make bench: sebesség-mérés -O2-ben, sanitizerek NÉLKÜL (ASan torzítana).
bench:
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_DIR)/map_bench.c $(SRC_DIR)/utils/map_khash.c $(SRC_DIR)/utils/arena.c $(BENCH_CFLAGS) -o $(TEST_BUILD)/bench_khash
	$(CC) $(TEST_DIR)/map_bench.c $(SRC_DIR)/utils/map_arena.c $(SRC_DIR)/utils/arena.c $(BENCH_CFLAGS) -o $(TEST_BUILD)/bench_arena
	$(TEST_BUILD)/bench_khash khash
	@echo
	$(TEST_BUILD)/bench_arena arena

.PHONY: all debug release run clean rebuild compdb test test-asan install-zed analyze bench fuzz

-include $(DEPS)
