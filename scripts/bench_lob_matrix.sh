#!/usr/bin/env bash
# Targeted benchmark sweep: 4 key configurations
#   500k/3t, 500k/8t, 500k/16t, 5M/16t
# Measures scaling across thread counts (1, 2, 4, 8) against sequential baseline
# Usage: ./bench_lob_matrix.sh [-v] [-grain {coarse|fine|batching}] [-workload {balanced|crossing|resting|skewed}] [-skew-ratio <0-1>]
#   -v: verbose output per cell; default is compact summary only
#   -grain: select engine (coarse, fine, or batching); default is coarse
#   -workload: order mix (balanced | crossing | resting | skewed); default balanced
#   -skew-ratio: skew ratio for skewed workload (0-1); default 0.9
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
    -v) VERBOSE=1; BENCH_ARGS="$BENCH_ARGS -v"; shift ;;
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
      BENCH_ARGS="$BENCH_ARGS -grain $GRAIN"
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
      BENCH_ARGS="$BENCH_ARGS -workload $WORKLOAD"
      shift 2
      ;;
    -skew-ratio)
      if [[ $# -lt 2 ]]; then
        echo "usage: -skew-ratio <0-1>" >&2
        exit 1
      fi
      BENCH_ARGS="$BENCH_ARGS -skew-ratio $2"
      shift 2
      ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

if [[ ! -x "$BENCH" ]]; then
  echo "error: $BENCH not found or not executable; run 'chmod +x scripts/bench_lob.sh'" >&2
  exit 1
fi

# Targeted configurations: (orders, tickers) pairs
configs=(
  "500000:3"
  "500000:8"
  "500000:16"
  "5000000:16"
)
thread_counts=(1 2 4 8)

mkdir -p "$RESULTS_DIR"
: > "$LOG_FILE"

log() {
  printf '%s\n' "$*" | tee -a "$LOG_FILE"
}

# Associative array: results["500000_3_seq"] = speedup
declare -A results

if [[ "$VERBOSE" == "1" ]]; then
  log "Running targeted benchmark sweep (4 configs) with verbose output (grain=$GRAIN workload=$WORKLOAD)"
else
  echo "Running targeted benchmark (4 configs, compact output; use -v for details) [grain=$GRAIN workload=$WORKLOAD]"
fi
log "Configurations: 500k/3t, 500k/8t, 500k/16t, 5M/16t"
log "Log file: $LOG_FILE"
log ""

for config in "${configs[@]}"; do
  IFS=':' read -r n t <<< "$config"
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

if [[ "$VERBOSE" == "0" ]]; then
  echo ""
  echo "=== Speedup Summary ==="
  echo ""
  printf '%-12s %8s %8s %8s %8s %8s\n' "Config" "seq" "1-thr" "2-thr" "4-thr" "8-thr"
  printf '%-12s %8s %8s %8s %8s %8s\n' "------------" "--------" "--------" "--------" "--------" "--------"

  for config in "${configs[@]}"; do
    IFS=':' read -r n t <<< "$config"
    n_label=$(printf '%s' "$n" | sed 's/000000/M/; s/000$/k/')
    config_label="${n_label}/${t}t"
    seq_sp=${results["${n}_${t}_seq"]:-1.00}
    sp1=${results["${n}_${t}_1"]:-n/a}
    sp2=${results["${n}_${t}_2"]:-n/a}
    sp4=${results["${n}_${t}_4"]:-n/a}
    sp8=${results["${n}_${t}_8"]:-n/a}
    printf '%-12s %8s %8s %8s %8s %8s\n' "$config_label" "1.00" "$sp1" "$sp2" "$sp4" "$sp8"
  done
  echo ""
fi

log ""
log "Matrix sweep complete."
