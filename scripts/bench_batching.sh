#!/usr/bin/env bash
# Compare batching engine with coarse and fine engines

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/build/sim"

if [[ ! -x "$BIN" ]]; then
  echo "error: $BIN not executable; run 'make' first" >&2
  exit 1
fi

SEED=42
NUM_ORDERS="${1:-500000}"
WORKLOAD="${2:-balanced}"
THREADS="${3:-8}"

echo "=========================================="
echo "Batching Engine Benchmark"
echo "=========================================="
echo ""
echo "Config: $NUM_ORDERS orders, $WORKLOAD workload, $THREADS threads"
echo ""

run_engine() {
  local engine=$1
  printf "%-12s" "$engine:"

  if [[ $THREADS -eq 1 ]]; then
    out=$("$BIN" --seed "$SEED" --num-orders "$NUM_ORDERS" --workload "$WORKLOAD" --engine "$engine" 2>&1)
  else
    out=$("$BIN" --seed "$SEED" --num-orders "$NUM_ORDERS" --workload "$WORKLOAD" --engine "$engine" --parallel --threads "$THREADS" 2>&1)
  fi

  time_us=$(echo "$out" | sed -n 's/.*Processed in \([0-9]*\) us.*/\1/p')
  echo "$time_us µs"
}

# Correctness check
echo "--- CORRECTNESS CHECK ---"
"$BIN" --seed 42 --num-orders 10000 --workload "$WORKLOAD" --engine coarse --dump-trades /tmp/coarse.json > /dev/null 2>&1
"$BIN" --seed 42 --num-orders 10000 --workload "$WORKLOAD" --engine batching --dump-trades /tmp/batching.json > /dev/null 2>&1

if diff -q /tmp/coarse.json /tmp/batching.json > /dev/null; then
  echo "✓ Trades match between coarse and batching"
else
  echo "✗ FAIL: Trades differ!"
  exit 1
fi

echo ""
echo "--- PERFORMANCE COMPARISON ---"

# Run each engine and capture timing
coarse_out=$("$BIN" --seed "$SEED" --num-orders "$NUM_ORDERS" --workload "$WORKLOAD" --engine coarse $([ "$THREADS" -gt 1 ] && echo "--parallel --threads $THREADS") 2>&1)
batching_out=$("$BIN" --seed "$SEED" --num-orders "$NUM_ORDERS" --workload "$WORKLOAD" --engine batching $([ "$THREADS" -gt 1 ] && echo "--parallel --threads $THREADS") 2>&1)
fine_out=$("$BIN" --seed "$SEED" --num-orders "$NUM_ORDERS" --workload "$WORKLOAD" --engine fine $([ "$THREADS" -gt 1 ] && echo "--parallel --threads $THREADS") 2>&1)

# Extract timing
coarse_time=$(echo "$coarse_out" | sed -n 's/.*Processed in \([0-9]*\) us.*/\1/p')
batching_time=$(echo "$batching_out" | sed -n 's/.*Processed in \([0-9]*\) us.*/\1/p')
fine_time=$(echo "$fine_out" | sed -n 's/.*Processed in \([0-9]*\) us.*/\1/p')

echo "coarse:     $coarse_time µs"
echo "batching:   $batching_time µs"
echo "fine:       $fine_time µs"

echo ""
echo "--- SPEEDUP ANALYSIS ---"
echo ""

if [[ -n "$coarse_time" && -n "$batching_time" && "$batching_time" != "0" ]]; then
  speedup_coarse=$(awk -v c="$coarse_time" -v b="$batching_time" 'BEGIN {printf "%.2f", c/b}')
  echo "Speedup vs coarse:  $speedup_coarse x (>1.0 = batching faster)"
else
  echo "Speedup vs coarse:  error (timing unavailable)"
fi

if [[ -n "$fine_time" && -n "$batching_time" && "$batching_time" != "0" ]]; then
  speedup_fine=$(awk -v f="$fine_time" -v b="$batching_time" 'BEGIN {printf "%.2f", f/b}')
  echo "Speedup vs fine:    $speedup_fine x (>1.0 = batching faster)"
else
  echo "Speedup vs fine:    error (timing unavailable)"
fi
