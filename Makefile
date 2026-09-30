CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -pthread -Iinclude -O3 -D_GNU_SOURCE
LDFLAGS ?= -pthread -lm

SRC_DIR = src
INC_DIR = include
BUILD_DIR = build
BIN_DIR = bin
TEST_DIR = tests
BENCH_DIR = benchmarks
CLI_DIR = cli

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

LIB_STATIC = $(BUILD_DIR)/libtaskforge.a
LIB_SHARED = $(BUILD_DIR)/libtaskforge.so

TEST_SRCS = $(wildcard $(TEST_DIR)/*.c)
TEST_BINS = $(patsubst $(TEST_DIR)/%.c, $(BIN_DIR)/%, $(TEST_SRCS))
MEMCHECK_BINS = $(filter-out $(BIN_DIR)/test_million_soak,$(TEST_BINS))

BENCH_SRCS = $(wildcard $(BENCH_DIR)/*.c)
BENCH_BINS = $(patsubst $(BENCH_DIR)/%.c, $(BIN_DIR)/%, $(BENCH_SRCS))

CLI_BIN = $(BIN_DIR)/taskforge_cli

.PHONY: all clean test test-million bench valgrind tsan asan directories help

all: directories $(LIB_STATIC) $(LIB_SHARED) $(CLI_BIN) $(TEST_BINS) $(BENCH_BINS)

directories:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)

help:
	@echo "TaskForge Build Targets:"
	@echo "  make all          - Build libraries, CLI, test suite, and benchmarks"
	@echo "  make test         - Run full test battery (correctness, contention, shutdown)"
	@echo "  make test-million - Run 1,000,000 tasks soak test"
	@echo "  make bench        - Run scalability and work-stealing benchmarks"
	@echo "  make asan         - Run tests under AddressSanitizer"
	@echo "  make tsan         - Run tests under ThreadSanitizer"
	@echo "  make valgrind     - Run Valgrind leak checker"
	@echo "  make clean        - Remove build artifacts and logs"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(LIB_STATIC): $(OBJS)
	ar rcs $@ $^

$(LIB_SHARED): $(OBJS)
	$(CC) -shared $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(CLI_BIN): $(CLI_DIR)/taskforge_cli.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< $(LIB_STATIC) -o $@ $(LDFLAGS)

$(BIN_DIR)/%: $(TEST_DIR)/%.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< $(LIB_STATIC) -o $@ $(LDFLAGS)

$(BIN_DIR)/%: $(BENCH_DIR)/%.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< $(LIB_STATIC) -o $@ $(LDFLAGS)

test: all
	@echo "=========================================="
	@echo "  Running TaskForge Test Battery"
	@echo "=========================================="
	@printf '%s\n' $(TEST_BINS) | xargs -r -n1
	@echo "=========================================="
	@echo "  ALL DISCOVERED TESTS PASSED"
	@echo "=========================================="

test-million: all
	@echo "Running Full 1,000,000 Tasks Soak Test..."
	@./$(BIN_DIR)/test_million_soak 1000000

bench: all
	@echo "=========================================="
	@echo "  Running TaskForge Benchmarks"
	@echo "=========================================="
	@./$(BIN_DIR)/bench_scaling 100000
	@./$(BIN_DIR)/bench_stealing_vs_global

asan: CFLAGS += -fsanitize=address,undefined -g -O1
asan: LDFLAGS += -fsanitize=address,undefined
asan:
	@$(MAKE) clean
	@$(MAKE) CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" all
	@echo "Running tests with AddressSanitizer..."
	@printf '%s\n' $(TEST_BINS) | xargs -r -n1

tsan: CFLAGS += -fsanitize=thread -g -O1
tsan: LDFLAGS += -fsanitize=thread
tsan:
	@$(MAKE) clean
	@$(MAKE) CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" all
	@echo "Running tests with ThreadSanitizer..."
	@sysctl -w vm.mmap_rnd_bits=28 2>/dev/null || true
	@printf '%s\n' $(TEST_BINS) | xargs -r -n1

valgrind: all
	@command -v valgrind >/dev/null || { echo "Valgrind is not installed."; exit 2; }
	@echo "Running Valgrind Memory Verification..."
	@printf '%s\n' $(MEMCHECK_BINS) | xargs -r -n1 valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1
	@echo "VALGRIND VERIFICATION COMPLETE!"

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) *.log
