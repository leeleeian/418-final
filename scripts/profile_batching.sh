#!/usr/bin/env bash
# Comprehensive profiling of batching vs coarse/fine engines
# Captures: cycles, instructions, IPC, cache misses, L1 misses
#
# Usage: ./scripts/profile_batching.sh [--quick|--full]

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
WORKLOADS=(balanced crossing resting skewed)
ORDER_COUNTS=(500000)  # Focus on 500k orders for detailed analysis
TICKERS=(8)
THREAD_COUNTS=(1 2 4 8)
SEED=42

echo "=========================================="
echo "Batching Engine Profiling (with perf stats)"
echo "=========================================="
echo ""
echo "Mode: $MODE"
echo "Workloads: ${WORKLOADS[@]}"
echo "Orders: ${ORDER_COUNTS[@]}"
echo "Tickers: ${TICKERS[@]}"
echo "Threads: ${THREAD_COUNTS[@]}"
echo ""

run_with_perf() {
  local engine=$1 workload=$2 orders=$3 tickers=$4 threads=$5

  local perf_events="cycles,instructions,cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses"
  local cmd="perf stat -e $perf_events"

  cmd="$cmd $BIN --seed $SEED --num-orders $orders --num-tickers $tickers --workload $workload"

  # Add skew-ratio for skewed workload
  if [[ "$workload" == "skewed" ]]; then
    cmd="$cmd --skew-ratio 0.9"
  fi

  # Add engine and parallel flags
  if [[ $threads -eq 1 ]]; then
    cmd="$cmd --engine $engine"
  else
    cmd="$cmd --engine $engine --parallel --threads $threads"
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

# CSV file for raw data
CSV="${RESULTS_DIR}/profile_batching.csv"
cat > "$CSV" << 'HEADER'
workload,orders,tickers,threads,engine,wall_time_us,cycles,instructions,ipc,cache_refs,cache_misses,cache_miss_rate,l1_loads,l1_misses,l1_miss_rate
HEADER

echo "Running profiling tests..."
echo ""

total_tests=0
for workload in "${WORKLOADS[@]}"; do
  for orders in "${ORDER_COUNTS[@]}"; do
    for tickers in "${TICKERS[@]}"; do
      for threads in "${THREAD_COUNTS[@]}"; do
        total_tests=$((total_tests + 1))
      done
    done
  done
done

test_num=0

for workload in "${WORKLOADS[@]}"; do
  for orders in "${ORDER_COUNTS[@]}"; do
    for tickers in "${TICKERS[@]}"; do
      for threads in "${THREAD_COUNTS[@]}"; do
        test_num=$((test_num + 1))
        pct=$((100 * test_num / total_tests))

        printf "\r[%3d%%] %s / %s orders / %dt" $pct "$workload" "$orders" "$threads"

        for engine in coarse fine batching; do
          output=$(run_with_perf "$engine" "$workload" "$orders" "$tickers" "$threads" 2>&1)

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

          echo "$workload,$orders,$tickers,$threads,$engine,$wall_time,$cycles,$insn,$ipc,$cache_refs,$cache_misses,$cache_miss_rate,$l1_loads,$l1_misses,$l1_miss_rate" >> "$CSV"
        done
      done
    done
  done
done

echo ""
echo ""
echo "✓ Raw data saved to: $CSV"
echo ""

# Generate analysis report
REPORT="${RESULTS_DIR}/profile_batching_report.txt"
cat > "$REPORT" << 'EOF'
BATCHING ENGINE PROFILING REPORT
================================================================================

This report analyzes the batching engine performance relative to coarse/fine
using detailed perf stats: cycles, instructions, IPC, cache misses.

The goal is to understand whether batching underperformance is due to:
  - Worse instruction efficiency (lower IPC)
  - More cache misses
  - More cycles per operation
  - wouldCross() checking overhead

================================================================================
EOF

# Add summary tables
cat >> "$REPORT" << EOF

SUMMARY BY WORKLOAD
================================================================================

EOF

for workload in "${WORKLOADS[@]}"; do
  echo "Workload: $workload" >> "$REPORT"
  echo "--------" >> "$REPORT"
  echo "" >> "$REPORT"
  echo "Wall Time Comparison (µs):" >> "$REPORT"
  echo "Threads | Coarse  | Fine    | Batching | Fine vs Coarse | Batching vs Coarse" >> "$REPORT"
  echo "--------|---------|---------|----------|----------------|-------------------" >> "$REPORT"

  for threads in "${THREAD_COUNTS[@]}"; do
    coarse_time=$(grep "^$workload,500000,8,$threads,coarse," "$CSV" | cut -d, -f6)
    fine_time=$(grep "^$workload,500000,8,$threads,fine," "$CSV" | cut -d, -f6)
    batch_time=$(grep "^$workload,500000,8,$threads,batching," "$CSV" | cut -d, -f6)

    if [[ -n "$coarse_time" && -n "$fine_time" && -n "$batch_time" ]]; then
      fine_speedup=$(awk -v c="$coarse_time" -v f="$fine_time" 'BEGIN {printf "%.2f", c/f}')
      batch_speedup=$(awk -v c="$coarse_time" -v b="$batch_time" 'BEGIN {printf "%.2f", c/b}')
      printf "%-8d| %7s | %7s | %8s | %14s | %17s\n" "$threads" "$coarse_time" "$fine_time" "$batch_time" "$fine_speedup x" "$batch_speedup x" >> "$REPORT"
    fi
  done

  echo "" >> "$REPORT"
  echo "IPC (Instructions Per Cycle):" >> "$REPORT"
  echo "Threads | Coarse | Fine   | Batching" >> "$REPORT"
  echo "--------|--------|--------|----------" >> "$REPORT"

  for threads in "${THREAD_COUNTS[@]}"; do
    coarse_ipc=$(grep "^$workload,500000,8,$threads,coarse," "$CSV" | cut -d, -f9)
    fine_ipc=$(grep "^$workload,500000,8,$threads,fine," "$CSV" | cut -d, -f9)
    batch_ipc=$(grep "^$workload,500000,8,$threads,batching," "$CSV" | cut -d, -f9)

    if [[ -n "$coarse_ipc" && -n "$fine_ipc" && -n "$batch_ipc" ]]; then
      printf "%-8d| %6s | %6s | %8s\n" "$threads" "$coarse_ipc" "$fine_ipc" "$batch_ipc" >> "$REPORT"
    fi
  done

  echo "" >> "$REPORT"
  echo "Cache Miss Rate (% of cache refs):" >> "$REPORT"
  echo "Threads | Coarse | Fine   | Batching" >> "$REPORT"
  echo "--------|--------|--------|----------" >> "$REPORT"

  for threads in "${THREAD_COUNTS[@]}"; do
    coarse_cmr=$(grep "^$workload,500000,8,$threads,coarse," "$CSV" | cut -d, -f12)
    fine_cmr=$(grep "^$workload,500000,8,$threads,fine," "$CSV" | cut -d, -f12)
    batch_cmr=$(grep "^$workload,500000,8,$threads,batching," "$CSV" | cut -d, -f12)

    if [[ -n "$coarse_cmr" && -n "$fine_cmr" && -n "$batch_cmr" ]]; then
      printf "%-8d| %5.1f%% | %5.1f%% | %6.1f%%\n" "$threads" "$coarse_cmr" "$fine_cmr" "$batch_cmr" >> "$REPORT"
    fi
  done

  echo "" >> "$REPORT"
  echo "L1 Data Cache Miss Rate (% of loads):" >> "$REPORT"
  echo "Threads | Coarse | Fine   | Batching" >> "$REPORT"
  echo "--------|--------|--------|----------" >> "$REPORT"

  for threads in "${THREAD_COUNTS[@]}"; do
    coarse_l1=$(grep "^$workload,500000,8,$threads,coarse," "$CSV" | cut -d, -f15)
    fine_l1=$(grep "^$workload,500000,8,$threads,fine," "$CSV" | cut -d, -f15)
    batch_l1=$(grep "^$workload,500000,8,$threads,batching," "$CSV" | cut -d, -f15)

    if [[ -n "$coarse_l1" && -n "$fine_l1" && -n "$batch_l1" ]]; then
      printf "%-8d| %5.1f%% | %5.1f%% | %6.1f%%\n" "$threads" "$coarse_l1" "$fine_l1" "$batch_l1" >> "$REPORT"
    fi
  done

  echo "" >> "$REPORT"
  echo "Cycles (absolute):" >> "$REPORT"
  echo "Threads | Coarse    | Fine      | Batching" >> "$REPORT"
  echo "--------|-----------|-----------|----------" >> "$REPORT"

  for threads in "${THREAD_COUNTS[@]}"; do
    coarse_cyc=$(grep "^$workload,500000,8,$threads,coarse," "$CSV" | cut -d, -f7)
    fine_cyc=$(grep "^$workload,500000,8,$threads,fine," "$CSV" | cut -d, -f7)
    batch_cyc=$(grep "^$workload,500000,8,$threads,batching," "$CSV" | cut -d, -f7)

    if [[ -n "$coarse_cyc" && -n "$fine_cyc" && -n "$batch_cyc" ]]; then
      printf "%-8d| %9s | %9s | %9s\n" "$threads" "$coarse_cyc" "$fine_cyc" "$batch_cyc" >> "$REPORT"
    fi
  done

  echo "" >> "$REPORT"
  echo "" >> "$REPORT"
done

echo "=================================================================================" >> "$REPORT"
echo "ANALYSIS" >> "$REPORT"
echo "=================================================================================" >> "$REPORT"
echo "" >> "$REPORT"
echo "Key findings:" >> "$REPORT"
echo "" >> "$REPORT"
echo "1. If IPC is similar: Performance difference is due to cache/memory behavior" >> "$REPORT"
echo "2. If IPC is worse: Batching code has higher instruction overhead" >> "$REPORT"
echo "3. If L1 miss rate is higher: Batching has worse cache locality" >> "$REPORT"
echo "4. If L1 miss rate similar: Problem is deeper (LLC, memory bandwidth)" >> "$REPORT"
echo "" >> "$REPORT"

cat "$REPORT"
echo ""
echo "✓ Report saved to: $REPORT"
