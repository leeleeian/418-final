#!/usr/bin/env bash
# Profile the 4 key configurations across workloads
# Collects: cycles, instructions, IPC, cache-misses, L1-dcache miss rate
# Usage: ./scripts/profile_matrix.sh [-grain {coarse|fine|batching}] [-workload {balanced|crossing|resting|skewed}] [-skew-ratio <0-1>]

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/build/sim"
RESULTS_DIR="${ROOT}/results"

if [[ ! -x "$BIN" ]]; then
  echo "error: $BIN not executable; run 'make' first" >&2
  exit 1
fi

mkdir -p "$RESULTS_DIR"

# Defaults
GRAIN="coarse"
WORKLOAD="balanced"
SKEW_RATIO=0.9

# Parse flags
while [[ $# -gt 0 ]]; do
  case "$1" in
    -grain)
      if [[ $# -lt 2 ]]; then
        echo "usage: -grain {coarse|fine|batching}" >&2
        exit 1
      fi
      GRAIN="$2"
      if [[ "$GRAIN" != "coarse" && "$GRAIN" != "fine" && "$GRAIN" != "batching" ]]; then
        echo "error: -grain must be 'coarse', 'fine', or 'batching'" >&2
        exit 1
      fi
      shift 2
      ;;
    -workload)
      if [[ $# -lt 2 ]]; then
        echo "usage: -workload {balanced|crossing|resting|skewed}" >&2
        exit 1
      fi
      WORKLOAD="$2"
      if [[ "$WORKLOAD" != "balanced" && "$WORKLOAD" != "crossing" && "$WORKLOAD" != "resting" && "$WORKLOAD" != "skewed" ]]; then
        echo "error: -workload must be 'balanced', 'crossing', 'resting', or 'skewed'" >&2
        exit 1
      fi
      shift 2
      ;;
    -skew-ratio)
      if [[ $# -lt 2 ]]; then
        echo "usage: -skew-ratio <0-1>" >&2
        exit 1
      fi
      SKEW_RATIO="$2"
      shift 2
      ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

# Targeted configurations: (orders, tickers) pairs
configs=(
  "500000:3"
  "500000:8"
  "500000:16"
  "5000000:16"
)

thread_counts=(1 2 4 8)
SEED=42

echo "=========================================="
echo "Profile Matrix: $GRAIN / $WORKLOAD"
echo "=========================================="
echo ""
echo "Configurations: 500k/3t, 500k/8t, 500k/16t, 5M/16t"
echo "Thread counts: 1, 2, 4, 8"
echo ""

# Print header
printf '%-10s %-8s %-12s %-8s %-10s %-10s %-10s\n' "Config" "Threads" "Wall(µs)" "IPC" "CacheMiss%" "L1Miss%" "Cycles"
printf '%-10s %-8s %-12s %-8s %-10s %-10s %-10s\n' "----------" "--------" "--------" "------" "----------" "------" "----------"

run_with_perf() {
  local orders=$1 tickers=$2 threads=$3

  local perf_events="cycles,instructions,cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses"
  local cmd="perf stat -e $perf_events"

  cmd="$cmd $BIN --seed $SEED --num-orders $orders --num-tickers $tickers --workload $WORKLOAD"

  # Add skew-ratio for skewed workload
  if [[ "$WORKLOAD" == "skewed" ]]; then
    cmd="$cmd --skew-ratio $SKEW_RATIO"
  fi

  # Add engine and parallel flags
  if [[ $threads -eq 1 ]]; then
    cmd="$cmd --engine $GRAIN"
  else
    cmd="$cmd --engine $GRAIN --parallel --threads $threads"
  fi

  eval "$cmd 2>&1"
}

extract_metric() {
  local output=$1 metric=$2
  echo "$output" | grep "$metric" | head -1 | awk '{print $1}' | sed 's/,//g'
}

extract_wall_time() {
  local output=$1
  echo "$output" | grep "Processed in" | awk '{print $3}'
}

total_tests=0
for config in "${configs[@]}"; do
  for tc in "${thread_counts[@]}"; do
    total_tests=$((total_tests + 1))
  done
done

test_num=0

for config in "${configs[@]}"; do
  IFS=':' read -r n t <<< "$config"
  n_label=$(printf '%s' "$n" | sed 's/000000/M/; s/000$/k/')
  config_label="${n_label}/${t}t"

  for tc in "${thread_counts[@]}"; do
    test_num=$((test_num + 1))
    pct=$((100 * test_num / total_tests))

    printf "\r[%3d%%] Profiling..." $pct

    output=$(run_with_perf "$n" "$t" "$tc" 2>&1)

    wall_time=$(extract_wall_time "$output")
    cycles=$(extract_metric "$output" "cycles")
    insn=$(extract_metric "$output" "instructions")
    cache_refs=$(extract_metric "$output" "cache-references")
    cache_misses=$(extract_metric "$output" "cache-misses")
    l1_loads=$(extract_metric "$output" "L1-dcache-loads")
    l1_misses=$(extract_metric "$output" "L1-dcache-load-misses")

    # Compute derived metrics
    ipc=$(awk "BEGIN {if ($cycles > 0) printf \"%.3f\", $insn / $cycles; else print \"0\"}")
    cache_miss_rate=$(awk "BEGIN {if ($cache_refs > 0) printf \"%.2f\", 100.0 * $cache_misses / $cache_refs; else print \"0\"}")
    l1_miss_rate=$(awk "BEGIN {if ($l1_loads > 0) printf \"%.2f\", 100.0 * $l1_misses / $l1_loads; else print \"0\"}")

    # Print formatted result
    printf '%-10s %-8d %-12s %-8s %-10s %-10s %-10s\n' "$config_label" "$tc" "$wall_time" "$ipc" "$cache_miss_rate%" "$l1_miss_rate%" "$cycles"
  done
done

echo ""
