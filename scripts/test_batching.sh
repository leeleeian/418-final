#!/usr/bin/env bash
# Test batching matching engine against coarse and fine engines
# Usage: ./scripts/test_batching.sh [--quick|--full]

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/build/sim"
RESULTS_DIR="${ROOT}/results"

if [[ ! -x "$BIN" ]]; then
  echo "error: $BIN not executable; run 'make' first" >&2
  exit 1
fi

mkdir -p "$RESULTS_DIR"

MODE="${1:-quick}"

echo "=========================================="
echo "Batching Engine Testing"
echo "=========================================="
echo ""

# Test parameters
ORDER_COUNTS=(100000 500000 5000000)
WORKLOADS=(balanced crossing resting)
THREAD_COUNTS=(1 2 4 8)
SEED=42

run_test() {
  local engine=$1 workload=$2 orders=$3 threads=$4
  local label="${engine} | ${workload} | ${orders}ord | ${threads}t"

  printf "%-50s" "$label"

  if [[ $threads -eq 1 ]]; then
    out=$("$BIN" --seed "$SEED" --num-orders "$orders" --workload "$workload" --engine "$engine" 2>&1)
  else
    out=$("$BIN" --seed "$SEED" --num-orders "$orders" --workload "$workload" --engine "$engine" --parallel --threads "$threads" 2>&1)
  fi

  micros=$(echo "$out" | grep "Processed in" | awk '{print $3}')
  echo " $micros µs"
}

# Correctness test: all engines must produce same output for same seed
echo "--- CORRECTNESS TEST ---"
echo "Verifying all engines produce identical trades..."
echo ""

"$BIN" --seed 42 --num-orders 10000 --workload balanced --engine coarse --dump-trades /tmp/trades_coarse.json > /dev/null 2>&1
"$BIN" --seed 42 --num-orders 10000 --workload balanced --engine fine --dump-trades /tmp/trades_fine.json > /dev/null 2>&1
"$BIN" --seed 42 --num-orders 10000 --workload balanced --engine batching --dump-trades /tmp/trades_batching.json > /dev/null 2>&1

if diff -q /tmp/trades_coarse.json /tmp/trades_batching.json > /dev/null; then
  echo "✓ batching matches coarse (trades identical)"
else
  echo "✗ FAIL: batching does not match coarse"
  exit 1
fi

if diff -q /tmp/trades_fine.json /tmp/trades_batching.json > /dev/null; then
  echo "✓ batching matches fine (trades identical)"
else
  echo "✗ FAIL: batching does not match fine"
  exit 1
fi

echo ""
echo "--- PERFORMANCE COMPARISON ---"
echo ""

# Quick vs full mode
case "$MODE" in
  quick)
    echo "Mode: QUICK (1 order count × 3 workloads × 2 thread counts)"
    test_configs=(
      "balanced 500000 1"
      "balanced 500000 8"
      "crossing 500000 1"
      "crossing 500000 8"
      "resting 500000 1"
      "resting 500000 8"
    )
    ;;
  full)
    echo "Mode: FULL (3 order counts × 3 workloads × 4 thread counts)"
    test_configs=()
    for orders in "${ORDER_COUNTS[@]}"; do
      for workload in "${WORKLOADS[@]}"; do
        for threads in "${THREAD_COUNTS[@]}"; do
          test_configs+=("$workload $orders $threads")
        done
      done
    done
    ;;
  *)
    echo "error: unknown mode '$MODE'" >&2
    exit 1
    ;;
esac

# Run tests and collect results
echo ""
echo "Speedup = coarse_time / batching_time (>1.0 = batching faster)"
echo ""

declare -A results
total=${#test_configs[@]}
count=0

for config in "${test_configs[@]}"; do
  read workload orders threads <<< "$config"
  count=$((count + 1))
  pct=$((100 * count / total))

  printf "\r[%3d%%]" $pct

  # Coarse
  coarse_time=$(run_test coarse "$workload" "$orders" "$threads" 2>/dev/null | tail -1 | awk '{print $NF}' | sed 's/ µs//')

  # Batching
  batching_time=$(run_test batching "$workload" "$orders" "$threads" 2>/dev/null | tail -1 | awk '{print $NF}' | sed 's/ µs//')

  # Speedup
  if [[ -n "$coarse_time" && -n "$batching_time" ]]; then
    speedup=$(awk -v c="$coarse_time" -v b="$batching_time" 'BEGIN {printf "%.2f", c/b}')
  else
    speedup="n/a"
  fi

  key="${workload}:${orders}:${threads}"
  results["$key"]="$speedup"
done

echo -e "\r                 "
echo ""
echo "--- RESULTS SUMMARY ---"
echo ""

for config in "${test_configs[@]}"; do
  read workload orders threads <<< "$config"
  key="${workload}:${orders}:${threads}"
  speedup="${results[$key]:-n/a}"

  label=$(printf '%s' "$orders" | sed 's/000000/M/; s/000$/k/')
  printf "%-12s %-10s %-2dt  speedup: %s\n" "$workload" "$label" "$threads" "$speedup"
done

echo ""
echo "=========================================="
echo "Test complete!"
