#!/usr/bin/env bash
# Full benchmark matrix sweep:
#   NUM_ORDERS in {100k, 500k, 5M}
#   NUM_TICKERS in {3, 8, 16}
# Usage: ./bench_lob_matrix.sh [-v] [-grain {coarse|fine}] [-workload {balanced|crossing|resting}]
#   -v: verbose output per cell; default is compact summary only
#   -grain: select engine (coarse or fine); default is coarse
#   -workload: order mix (balanced | crossing | resting); default balanced
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH="${ROOT}/scripts/bench_lob.sh"
RESULTS_DIR="${ROOT}/results"
LOG_FILE="${LOG_FILE:-${RESULTS_DIR}/bench_lob_matrix.log}"
VERBOSE=0
GRAIN="coarse"
WORKLOAD="balanced"
BENCH_ARGS=""

# Parse flags
while [[ $# -gt 0 ]]; do
  case "$1" in
    -v) VERBOSE=1; BENCH_ARGS="-v"; shift ;;
    -grain)
      if [[ $# -lt 2 ]]; then
        echo "usage: -grain {coarse|fine}" >&2
        exit 1
      fi
      GRAIN="$2"
      if [[ "$GRAIN" != "coarse" && "$GRAIN" != "fine" ]]; then
        echo "error: -grain must be 'coarse' or 'fine'" >&2
        exit 1
      fi
      BENCH_ARGS="$BENCH_ARGS -grain $GRAIN"
      shift 2
      ;;
    -workload)
      if [[ $# -lt 2 ]]; then
        echo "usage: -workload {balanced|crossing|resting}" >&2
        exit 1
      fi
      WORKLOAD="$2"
      if [[ "$WORKLOAD" != "balanced" && "$WORKLOAD" != "crossing" && "$WORKLOAD" != "resting" ]]; then
        echo "error: -workload must be 'balanced', 'crossing', or 'resting'" >&2
        exit 1
      fi
      BENCH_ARGS="$BENCH_ARGS -workload $WORKLOAD"
      shift 2
      ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

if [[ ! -x "$BENCH" ]]; then
  echo "error: $BENCH not found or not executable; run 'chmod +x scripts/bench_lob.sh'" >&2
  exit 1
fi

order_counts=(100000 500000 5000000)
ticker_counts=(3 8 16)
thread_counts=(1 2 4 8)

mkdir -p "$RESULTS_DIR"
: > "$LOG_FILE"

log() {
  printf '%s\n' "$*" | tee -a "$LOG_FILE"
}

# Associative array: results["100000_3_seq"] = speedup
declare -A results

if [[ "$VERBOSE" == "1" ]]; then
  log "Running full LOB matrix sweep with verbose output (grain=$GRAIN workload=$WORKLOAD)"
else
  echo "Running matrix sweep (compact output; use -v for details) [grain=$GRAIN workload=$WORKLOAD]"
fi
log "Log file: $LOG_FILE"
log ""

for n in "${order_counts[@]}"; do
  for t in "${ticker_counts[@]}"; do
    if [[ "$VERBOSE" == "1" ]]; then
      log "============================================================"
      log "Matrix cell: NUM_ORDERS=$n NUM_TICKERS=$t"
      log "============================================================"
      NUM_ORDERS="$n" NUM_TICKERS="$t" "$BENCH" $BENCH_ARGS 2>&1 | tee -a "$LOG_FILE"
      log ""
    else
      printf "  %s orders / %s tickers: " "$n" "$t"

      # Quiet mode: extract timings only
      out=$(NUM_ORDERS="$n" NUM_TICKERS="$t" "$BENCH" $BENCH_ARGS 2>&1)
      log "=== NUM_ORDERS=$n NUM_TICKERS=$t GRAIN=$GRAIN WORKLOAD=$WORKLOAD ==="
      log "$out"

      # Parse summary table from output
      base_us=$(printf '%s\n' "$out" | grep "sequential baseline" | awk '{print $2}')
      results["${n}_${t}_seq"]="${base_us}"
      printf "seq=%.2fs " "$(awk "BEGIN {printf \"%.2f\", $base_us / 1e6}")"

      for tc in "${thread_counts[@]}"; do
        speedup=$(printf '%s\n' "$out" | grep "$GRAIN parallel threads=$tc" | tail -n1 | awk '{print $NF}' | sed 's/x$//')
        if [[ -z "$speedup" || ! "$speedup" =~ ^[0-9.]+$ ]]; then
          speedup="n/a"
        fi
        results["${n}_${t}_${tc}"]="$speedup"
        printf "t%d=%s " "$tc" "$speedup"
      done
      printf '\n'
    fi
  done
done

if [[ "$VERBOSE" == "0" ]]; then
  echo ""
  echo "=== Summary Table ==="
  echo ""
  printf '%-12s %8s %8s %8s %8s %8s\n' "Config" "seq" "1-thr" "2-thr" "4-thr" "8-thr"
  printf '%-12s %8s %8s %8s %8s %8s\n' "------------" "--------" "--------" "--------" "--------" "--------"

  for n in "${order_counts[@]}"; do
    n_label=$(printf '%s' "$n" | sed 's/000000/M/; s/000$/k/')
    for t in "${ticker_counts[@]}"; do
      config="${n_label}/${t}t"
      seq_sp=${results["${n}_${t}_seq"]}
      sp1=${results["${n}_${t}_1"]}
      sp2=${results["${n}_${t}_2"]}
      sp4=${results["${n}_${t}_4"]}
      sp8=${results["${n}_${t}_8"]}
      printf '%-12s %8s %8s %8s %8s %8s\n' "$config" "1.00" "$sp1" "$sp2" "$sp4" "$sp8"
    done
  done
  echo ""
fi

log ""
log "Matrix sweep complete."
