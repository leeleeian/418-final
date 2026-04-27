#!/usr/bin/env bash
# Compare coarse-grained vs fine-grained engines across workload types.
# Shows how each engine performs on balanced/crossing/resting order mixes.
#
# Usage: ./scripts/compare_grains_by_workload.sh [--quick|--full]
#   --quick: 1 workload × 2 configs (3 order counts × 1 ticker count) [default]
#   --full:  3 workloads × 3 configs (3 order counts × 3 ticker counts)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH="${ROOT}/scripts/bench_lob.sh"

# Test parameters
ORDER_COUNTS=(100000 500000 5000000)
TICKER_COUNTS=(3 8 16)
WORKLOADS=(balanced crossing resting)
THREAD_COUNTS=(1 2 4 8)

MODE="quick"
[[ $# -gt 0 && "$1" == "--full" ]] && MODE="full"

# Build test matrix
declare -a configs
if [[ "$MODE" == "quick" ]]; then
  for n in "${ORDER_COUNTS[@]}"; do
    configs+=("$n 8")  # 8 tickers as middle ground
  done
else
  for n in "${ORDER_COUNTS[@]}"; do
    for t in "${TICKER_COUNTS[@]}"; do
      configs+=("$n $t")
    done
  done
fi

echo "Fine vs Coarse by Workload [$MODE mode]"
echo "Speedup format: coarse_time / fine_time (>1.0 = fine is faster)"
echo ""

for workload in "${WORKLOADS[@]}"; do
  echo "╔══════════════════════════════════════════════════════════════════════╗"
  echo "║ Workload: $workload"
  echo "╚══════════════════════════════════════════════════════════════════════╝"
  echo ""

  printf "%-14s" "Config"
  for tc in "${THREAD_COUNTS[@]}"; do
    printf " | %6d-t" "$tc"
  done
  echo ""
  printf "%-14s" "────────────"
  for _ in "${THREAD_COUNTS[@]}"; do
    printf " | ──────"
  done
  echo ""

  for config in "${configs[@]}"; do
    read n t <<< "$config"

    # Format config label
    if [[ $n -eq 100000 ]]; then nl="100k"
    elif [[ $n -eq 500000 ]]; then nl="500k"
    else nl="5M"; fi

    printf "%-14s" "$nl/$t"

    for tc in "${THREAD_COUNTS[@]}"; do
      # Get coarse speedup
      coarse_out=$(NUM_ORDERS="$n" NUM_TICKERS="$t" "$BENCH" -grain coarse -workload "$workload" 2>&1)

      if [[ $tc -eq 1 ]]; then
        coarse_line=$(echo "$coarse_out" | grep "coarse single-threaded")
      else
        coarse_line=$(echo "$coarse_out" | grep "coarse parallel threads=$tc")
      fi
      coarse_us=$(echo "$coarse_line" | sed -E 's/^[[:space:]]*.*[[:space:]]+([0-9]+)[[:space:]]+.*/\1/')

      # Get fine speedup
      fine_out=$(NUM_ORDERS="$n" NUM_TICKERS="$t" "$BENCH" -grain fine -workload "$workload" 2>&1)

      if [[ $tc -eq 1 ]]; then
        fine_line=$(echo "$fine_out" | grep "fine single-threaded")
      else
        fine_line=$(echo "$fine_out" | grep "fine parallel threads=$tc")
      fi
      fine_us=$(echo "$fine_line" | sed -E 's/^[[:space:]]*.*[[:space:]]+([0-9]+)[[:space:]]+.*/\1/')

      # Compute speedup
      if [[ -n "$coarse_us" && -n "$fine_us" && "$fine_us" =~ ^[0-9]+$ ]]; then
        speedup=$(awk -v c="$coarse_us" -v f="$fine_us" 'BEGIN {
          if (f > 0) printf "%.2f", c / f; else printf "n/a"
        }')
      else
        speedup="n/a"
      fi

      printf " | %6s" "$speedup"
    done

    echo ""
  done

  echo ""
done

echo "Legend: >1.0 = coarse is faster, <1.0 = fine is faster"
echo "Note: Workload types:"
echo "  - balanced: 60% limit, 20% market, 20% cancel (default)"
echo "  - crossing: 30% limit, 60% market, 10% cancel (high matching)"
echo "  - resting: 70% limit, 10% market, 20% cancel (high resting)"
