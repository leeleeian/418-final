# Top-level Makefile for the parallel LOB project.
#
# Usage:
#   make              # build  ./build/sim
#   make run          # build then run the end-to-end driver
#   make baseline     # (re)generate the golden trade tape + book snapshot
#	make bench        # run throughput benchmarks
#   make verify       # run current binary, diff its output against golden
#   make dump         # write orders/trades/books JSON under build/dump/
#   make clean        # remove build artifacts (does NOT touch golden/)
#
# Override flags on the command line, e.g.
#   make CXX=g++-13
#   make CXXFLAGS="-std=c++17 -O3 -DNDEBUG"
#   make OPENMP=1

OPENMP   ?= 0
CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
LDFLAGS  ?= -pthread

ifeq ($(OPENMP),1)
CXXFLAGS += -fopenmp
LDFLAGS  += -fopenmp
endif

SRC_DIR   := code
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
DUMP_DIR  := $(BUILD_DIR)/dump
BIN       := $(BUILD_DIR)/sim

GOLDEN_DIR := golden

# Knobs the baseline / verify targets pin so the golden file is reproducible.
# Bumping any of these invalidates the existing golden — re-run `make baseline`.
BASELINE_SEED       ?= 42
BASELINE_NUM_ORDERS ?= 50000

# Auto-discover every .cpp under code/. New files are picked up without
# editing this Makefile — just `make` again.
SRCS := $(shell find $(SRC_DIR) -name '*.cpp' -type f)
OBJS := $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all build run clean dump baseline verify bench

all: $(BIN)
build: $(BIN)

$(BIN): $(OBJS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@
	@echo "  -> $@"

# Pattern rule mirrors the source tree under build/obj/.
# -MMD -MP emits a .d sidecar so editing a header triggers a rebuild of the
# right .cpp files on the next `make`.
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

run: $(BIN)
	@echo "  -> running $(BIN)"
	@./$(BIN)

# Dump everything (orders, trades, final books) under build/dump/ for
# ad-hoc inspection. Same data the verify target uses.
dump: $(BIN)
	@mkdir -p $(DUMP_DIR)
	@./$(BIN) --seed $(BASELINE_SEED) --num-orders $(BASELINE_NUM_ORDERS) \
	          --dump-orders $(DUMP_DIR)/orders.json \
	          --dump-trades $(DUMP_DIR)/trades.json \
	          --dump-books  $(DUMP_DIR)/books.json
	@echo "  -> dumped to $(DUMP_DIR)/"

# Capture the current sequential output as the canonical correct answer.
# Run this once (or after a deliberate semantic change to matching). Commit
# the resulting golden/ directory.
baseline: $(BIN)
	@mkdir -p $(GOLDEN_DIR)
	@./$(BIN) --seed $(BASELINE_SEED) --num-orders $(BASELINE_NUM_ORDERS) \
	          --dump-trades $(GOLDEN_DIR)/trades.json \
	          --dump-books  $(GOLDEN_DIR)/books.json > $(GOLDEN_DIR)/run.log
	@echo "  -> wrote golden trace to $(GOLDEN_DIR)/"
	@echo "     trades.json  $$(wc -c < $(GOLDEN_DIR)/trades.json) bytes"
	@echo "     books.json   $$(wc -c < $(GOLDEN_DIR)/books.json) bytes"

# Diff the current binary's output against the checked-in golden trace.
# Returns non-zero on any mismatch — wire this into CI later.
# Week 2+ throughput sweeps (override SEED / NUM_ORDERS as needed).
bench: $(BIN)
	@./scripts/bench_lob.sh

verify: $(BIN)
	@if [ ! -f $(GOLDEN_DIR)/trades.json ] || [ ! -f $(GOLDEN_DIR)/books.json ]; then \
	  echo "no golden trace found; run 'make baseline' first" >&2; exit 1; \
	fi
	@mkdir -p $(DUMP_DIR)
	@./$(BIN) --seed $(BASELINE_SEED) --num-orders $(BASELINE_NUM_ORDERS) \
	          --dump-trades $(DUMP_DIR)/trades.json \
	          --dump-books  $(DUMP_DIR)/books.json > /dev/null
	@diff -q $(GOLDEN_DIR)/trades.json $(DUMP_DIR)/trades.json \
	  && diff -q $(GOLDEN_DIR)/books.json  $(DUMP_DIR)/books.json \
	  && echo "  OK  current binary matches golden trace" \
	  || { echo "  FAIL  output diverged from golden — run 'diff $(GOLDEN_DIR)/ $(DUMP_DIR)/' to inspect" >&2; exit 1; }

clean:
	rm -rf $(BUILD_DIR)

# Pull in header dependency info for incremental rebuilds.
-include $(DEPS)
