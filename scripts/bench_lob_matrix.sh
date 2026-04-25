#!/usr/bin/env bash
# Full benchmark matrix sweep:
#   NUM_ORDERS in {100k, 500k, 5M}
#   NUM_TICKERS in {3, 8, 16}
# Runs each combination once and prints results to terminal only.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH="${ROOT}/scripts/bench_lob.sh"
RESULTS_DIR="${ROOT}/results"
LOG_FILE="${LOG_FILE:-${RESULTS_DIR}/bench_lob_matrix.log}"

if [[ ! -x "$BENCH" ]]; then
  echo "error: $BENCH not found or not executable; run 'chmod +x scripts/bench_lob.sh'" >&2
  exit 1
fi

order_counts=(100000 500000 5000000)
ticker_counts=(3 8 16)

mkdir -p "$RESULTS_DIR"
: > "$LOG_FILE"

log() {
  printf '%s\n' "$*" | tee -a "$LOG_FILE"
}

log "Running full LOB matrix sweep (no repeats)"
log "Seed is inherited from env (default bench script seed if unset)"
log "Log file: $LOG_FILE"
log ""

for n in "${order_counts[@]}"; do
  for t in "${ticker_counts[@]}"; do
    log "============================================================"
    log "Matrix cell: NUM_ORDERS=$n NUM_TICKERS=$t"
    log "============================================================"
    NUM_ORDERS="$n" NUM_TICKERS="$t" "$BENCH" 2>&1 | tee -a "$LOG_FILE"
    log ""
  done
done

log "Matrix sweep complete."
