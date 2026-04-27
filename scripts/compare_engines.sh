#!/usr/bin/env bash
# Compare fine-grained vs coarse-grained engines across thread counts.
# Output: speedup matrix showing fine/coarse ratio for each config.
#
# Usage: ./scripts/compare_engines.sh [--quick|--full] [--workload {balanced|crossing|resting}]
#   --quick: 1 sample per order/ticker combo (default)
#   --full: all 9 configs with all thread counts
#   --custom <n_orders> <n_tickers>: single test case
#   --workload: order mix (default balanced)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/build/sim"

if [[ ! -x "$BIN" ]]; then
  echo "error: $BIN not executable; run 'make' first" >&2
  exit 1
fi

# Configuration: modify these to change test matrix
ORDER_COUNTS=(100000 500000 5000000)
TICKER_COUNTS=(3 8 16)
THREAD_COUNTS=(1 2 4 8)
SEED=42
WORKLOAD="balanced"

# Parse command-line mode
MODE="quick"
CUSTOM_ORDERS=""
CUSTOM_TICKERS=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --quick) MODE="quick"; shift ;;
    --full) MODE="full"; shift ;;
    --custom)
      if [[ $# -lt 3 ]]; then
        echo "usage: --custom <orders> <tickers>" >&2
        exit 1
      fi
      MODE="custom"
      CUSTOM_ORDERS="$2"
      CUSTOM_TICKERS="$3"
      shift 3
      ;;
    --workload)
      if [[ $# -lt 2 ]]; then
        echo "usage: --workload {balanced|crossing|resting}" >&2
        exit 1
      fi
      WORKLOAD="$2"
      if [[ "$WORKLOAD" != "balanced" && "$WORKLOAD" != "crossing" && "$WORKLOAD" != "resting" ]]; then
        echo "error: --workload must be balanced, crossing, or resting" >&2
        exit 1
      fi
      shift 2
      ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

# Build test configs based on mode
configs=()
case "$MODE" in
  quick)
    # One sample per order count (3 configs)
    for n in "${ORDER_COUNTS[@]}"; do
      configs+=("$n 8 $SEED")  # 8 tickers as middle ground
    done
    ;;
  full)
    # All 9 combinations (3x3)
    for n in "${ORDER_COUNTS[@]}"; do
      for t in "${TICKER_COUNTS[@]}"; do
        configs+=("$n $t $SEED")
      done
    done
    ;;
  custom)
    configs=("$CUSTOM_ORDERS $CUSTOM_TICKERS $SEED")
    ;;
esac

thread_counts=("${THREAD_COUNTS[@]}")

# Helper: run engine and extract elapsed time in microseconds
run_engine() {
  local engine=$1 num_orders=$2 num_tickers=$3 threads=$4 seed=$5
  if [[ $threads -eq 1 ]]; then
    "$BIN" --seed "$seed" --num-orders "$num_orders" --num-tickers "$num_tickers" \
           --workload "$WORKLOAD" --engine "$engine" 2>&1 | grep "Processed in" | awk '{print $3}'
  else
    "$BIN" --seed "$seed" --num-orders "$num_orders" --num-tickers "$num_tickers" \
           --workload "$WORKLOAD" --engine "$engine" --parallel --threads "$threads" 2>&1 | grep "Processed in" | awk '{print $3}'
  fi
}

echo "Fine-grained vs Coarse-grained Speedup Matrix [$MODE mode, workload=$WORKLOAD]"
echo "(speedup = coarse_time / fine_time; >1.0 means fine is faster)"
echo ""
printf "%-10s" "Config"
for tc in "${thread_counts[@]}"; do
  printf " | %4d-t" "$tc"
done
echo ""
line_len=$((10 + 12 * ${#thread_counts[@]}))
printf "%s\n" "$(printf '=%.0s' $(seq 1 "$line_len"))"

for config in "${configs[@]}"; do
  read num_orders num_tickers seed <<< "$config"

  # Format config name (abbreviate order count)
  if [[ $num_orders -eq 100000 ]]; then ord="100k"
  elif [[ $num_orders -eq 500000 ]]; then ord="500k"
  else ord="5M"; fi

  printf "%-10s" "$ord/$num_tickers"

  for tc in "${thread_counts[@]}"; do
    coarse=$(run_engine coarse "$num_orders" "$num_tickers" "$tc" "$seed")
    fine=$(run_engine fine "$num_orders" "$num_tickers" "$tc" "$seed")

    # Compute speedup using awk (no bc dependency)
    speedup=$(awk -v c="$coarse" -v f="$fine" 'BEGIN {
      if (f > 0) printf "%.2f", c / f; else printf "ERR"
    }')

    printf " | %6s" "$speedup"
  done

  echo ""
done

echo ""
echo "Note: Time unit is microseconds (us). Speedup >1.0 favors fine-grained."
