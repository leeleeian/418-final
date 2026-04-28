#!/usr/bin/env bash
# Comprehensive profiling of fine vs coarse-grained engines
# Tests: all workloads × order counts × thread counts
# Collects: wall time, cycles, instructions, cache misses, lock contention
#
# Usage: ./scripts/profile_engines_comprehensive.sh [--quick|--full] [--output results.csv]
#   --quick: 1 config per workload (100k/500k/5M × 8t) [default]
#   --full:  3 configs per workload (100k/500k/5M × 3/8/16t)
#   --output: save raw data to CSV for post-processing

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/build/sim"
RESULTS_DIR="${ROOT}/results"
REPORT_FILE="${RESULTS_DIR}/profile_comprehensive_report.txt"
CSV_FILE="${RESULTS_DIR}/profile_comprehensive.csv"

if [[ ! -x "$BIN" ]]; then
  echo "error: $BIN not executable; run 'make' first" >&2
  exit 1
fi

mkdir -p "$RESULTS_DIR"

# Configuration
MODE="quick"
WORKLOADS=(balanced crossing resting skewed)
ORDER_COUNTS=(100000 500000 5000000)
TICKER_COUNTS=(3 8 16)
THREAD_COUNTS=(1 2 4 8)
SEED=42
SKEW_RATIO=0.9  # 90% orders on first ticker for skewed workload

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    --quick) MODE="quick"; shift ;;
    --full) MODE="full"; shift ;;
    --output) CSV_FILE="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

# Helper: run engine with perf stat collection
run_with_perf() {
  local engine=$1 workload=$2 orders=$3 tickers=$4 threads=$5
  local cmd_base="perf stat -e cycles,instructions,cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses \
      $BIN --seed $SEED --num-orders $orders --num-tickers $tickers --workload $workload"

  # Add skew-ratio for skewed workload
  if [[ "$workload" == "skewed" ]]; then
    cmd_base="$cmd_base --skew-ratio $SKEW_RATIO"
  fi

  # Add engine and parallel flags
  if [[ $threads -eq 1 ]]; then
    cmd_base="$cmd_base --engine $engine"
  else
    cmd_base="$cmd_base --engine $engine --parallel --threads $threads"
  fi

  eval "$cmd_base 2>&1"
}

# Helper: extract metric from perf output
extract_metric() {
  local output=$1 metric=$2
  echo "$output" | grep "$metric" | awk '{print $1}' | sed 's/,//g' | head -1
}

# Helper: extract wall time
extract_wall_time() {
  local output=$1
  echo "$output" | grep "Processed in" | awk '{print $3}' | head -1
}

echo "Comprehensive Fine vs Coarse Profiling [$MODE mode]"
echo "================================================================"
echo ""
echo "Test matrix:"
echo "  Workloads: ${WORKLOADS[@]}"
echo "  Order counts: ${ORDER_COUNTS[@]}"
echo "  Ticker counts: ${TICKER_COUNTS[@]}"
echo "  Thread counts: ${THREAD_COUNTS[@]}"
echo ""

# Build test matrix
declare -a configs
case "$MODE" in
  quick)
    for workload in "${WORKLOADS[@]}"; do
      for orders in "${ORDER_COUNTS[@]}"; do
        configs+=("$workload $orders 8")
      done
    done
    ;;
  full)
    for workload in "${WORKLOADS[@]}"; do
      for orders in "${ORDER_COUNTS[@]}"; do
        for tickers in "${TICKER_COUNTS[@]}"; do
          configs+=("$workload $orders $tickers")
        done
      done
    done
    ;;
esac

# Write CSV header
cat > "$CSV_FILE" << 'CSVHEADER'
workload,orders,tickers,threads,engine,wall_time_us,cycles,instructions,ipc,cache_misses,l1_misses,cache_miss_rate
CSVHEADER

# Main test loop
total=${#configs[@]}
count=0

for config in "${configs[@]}"; do
  read workload orders tickers <<< "$config"

  count=$((count + 1))
  pct=$((100 * count / total))
  printf "\r[%d%%] Testing: $workload/$orders/$tickers" $pct

  for threads in "${THREAD_COUNTS[@]}"; do
    # Profile coarse-grained
    coarse_output=$(run_with_perf coarse "$workload" "$orders" "$tickers" "$threads")
    coarse_time=$(extract_wall_time "$coarse_output")
    coarse_cycles=$(extract_metric "$coarse_output" "cycles")
    coarse_insn=$(extract_metric "$coarse_output" "instructions")
    coarse_misses=$(extract_metric "$coarse_output" "cache-misses")
    coarse_l1=$(extract_metric "$coarse_output" "L1-dcache-load-misses")
    coarse_refs=$(extract_metric "$coarse_output" "cache-references")

    # Compute IPC and cache miss rate
    coarse_ipc=$(awk "BEGIN {if ($coarse_cycles > 0) printf \"%.2f\", $coarse_insn / $coarse_cycles; else print \"n/a\"}")
    coarse_miss_rate=$(awk "BEGIN {if ($coarse_refs > 0) printf \"%.2f\", 100.0 * $coarse_misses / $coarse_refs; else print \"n/a\"}")

    echo "$workload,$orders,$tickers,$threads,coarse,$coarse_time,$coarse_cycles,$coarse_insn,$coarse_ipc,$coarse_misses,$coarse_l1,$coarse_miss_rate" >> "$CSV_FILE"

    # Profile fine-grained
    fine_output=$(run_with_perf fine "$workload" "$orders" "$tickers" "$threads")
    fine_time=$(extract_wall_time "$fine_output")
    fine_cycles=$(extract_metric "$fine_output" "cycles")
    fine_insn=$(extract_metric "$fine_output" "instructions")
    fine_misses=$(extract_metric "$fine_output" "cache-misses")
    fine_l1=$(extract_metric "$fine_output" "L1-dcache-load-misses")
    fine_refs=$(extract_metric "$fine_output" "cache-references")

    # Compute IPC and cache miss rate
    fine_ipc=$(awk "BEGIN {if ($fine_cycles > 0) printf \"%.2f\", $fine_insn / $fine_cycles; else print \"n/a\"}")
    fine_miss_rate=$(awk "BEGIN {if ($fine_refs > 0) printf \"%.2f\", 100.0 * $fine_misses / $fine_refs; else print \"n/a\"}")

    echo "$workload,$orders,$tickers,$threads,fine,$fine_time,$fine_cycles,$fine_insn,$fine_ipc,$fine_misses,$fine_l1,$fine_miss_rate" >> "$CSV_FILE"
  done
done

echo ""
echo "✓ Raw data saved to: $CSV_FILE"
echo ""

# Generate report
echo "Comprehensive Profiling Report" > "$REPORT_FILE"
echo "Generated: $(date)" >> "$REPORT_FILE"
echo "================================================================" >> "$REPORT_FILE"
echo "" >> "$REPORT_FILE"

# For each workload, generate analysis
for workload in "${WORKLOADS[@]}"; do
  echo "WORKLOAD: $workload" >> "$REPORT_FILE"
  echo "================================================================" >> "$REPORT_FILE"

  # Summary table: speedup across all configs
  echo "" >> "$REPORT_FILE"
  echo "Speedup Summary (fine speedup ratio = coarse_time / fine_time):" >> "$REPORT_FILE"
  echo "Config (orders/tickers) | 1-thread | 2-thread | 4-thread | 8-thread" >> "$REPORT_FILE"
  echo "------------------------+----------+----------+----------+----------" >> "$REPORT_FILE"

  case "$MODE" in
    quick)
      for orders in "${ORDER_COUNTS[@]}"; do
        label=$(printf '%s' "$orders" | sed 's/000000/M/; s/000$/k/')
        printf "%-24s" "$label/8" >> "$REPORT_FILE"

        for threads in "${THREAD_COUNTS[@]}"; do
          coarse_time=$(grep "^$workload,$orders,8,$threads,coarse," "$CSV_FILE" | cut -d, -f6)
          fine_time=$(grep "^$workload,$orders,8,$threads,fine," "$CSV_FILE" | cut -d, -f6)

          if [[ -n "$coarse_time" && -n "$fine_time" && "$fine_time" != "0" ]]; then
            speedup=$(awk -v c="$coarse_time" -v f="$fine_time" 'BEGIN {printf "%.2f", c/f}')
          else
            speedup="n/a"
          fi

          printf " | %8s" "$speedup" >> "$REPORT_FILE"
        done
        echo "" >> "$REPORT_FILE"
      done
      ;;
    full)
      for orders in "${ORDER_COUNTS[@]}"; do
        for tickers in "${TICKER_COUNTS[@]}"; do
          label=$(printf '%s' "$orders" | sed 's/000000/M/; s/000$/k/')
          printf "%-24s" "$label/$tickers" >> "$REPORT_FILE"

          for threads in "${THREAD_COUNTS[@]}"; do
            coarse_time=$(grep "^$workload,$orders,$tickers,$threads,coarse," "$CSV_FILE" | cut -d, -f6)
            fine_time=$(grep "^$workload,$orders,$tickers,$threads,fine," "$CSV_FILE" | cut -d, -f6)

            if [[ -n "$coarse_time" && -n "$fine_time" && "$fine_time" != "0" ]]; then
              speedup=$(awk -v c="$coarse_time" -v f="$fine_time" 'BEGIN {printf "%.2f", c/f}')
            else
              speedup="n/a"
            fi

            printf " | %8s" "$speedup" >> "$REPORT_FILE"
          done
          echo "" >> "$REPORT_FILE"
        done
      done
      ;;
  esac

  # Cache analysis
  echo "" >> "$REPORT_FILE"
  echo "Cache Miss Rate (L1 dcache %):" >> "$REPORT_FILE"

  case "$MODE" in
    quick)
      for orders in "${ORDER_COUNTS[@]}"; do
        echo "" >> "$REPORT_FILE"
        label=$(printf '%s' "$orders" | sed 's/000000/M/; s/000$/k/')
        echo "  $label/8 tickers:" >> "$REPORT_FILE"

        for threads in 8; do
          coarse_rate=$(grep "^$workload,$orders,8,$threads,coarse," "$CSV_FILE" | cut -d, -f12)
          fine_rate=$(grep "^$workload,$orders,8,$threads,fine," "$CSV_FILE" | cut -d, -f12)

          printf "    threads=%d: coarse=%.2f%% fine=%.2f%%\n" "$threads" "$coarse_rate" "$fine_rate" >> "$REPORT_FILE"
        done
      done
      ;;
  esac

  echo "" >> "$REPORT_FILE"
  echo "" >> "$REPORT_FILE"
done

cat "$REPORT_FILE"
echo ""
echo "✓ Full report saved to: $REPORT_FILE"
