#!/usr/bin/env bash
# Throughput + wall-time speedup vs sequential baseline (same seed / num-orders).
# Usage: ./bench_lob.sh [-v]
#   -v: verbose output (full details); default is summary table only
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/build/sim"
SEED="${SEED:-42}"
NUM_ORDERS="${NUM_ORDERS:-500000}"
NUM_TICKERS="${NUM_TICKERS:-16}"
VERBOSE=0

# Parse -v flag
while [[ $# -gt 0 ]]; do
  case "$1" in
    -v) VERBOSE=1; shift ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

make -C "$ROOT" clean > /dev/null 2>&1
make -C "$ROOT" > /dev/null 2>&1

if [[ ! -x "$BIN" ]]; then
  echo "error: $BIN not found; run 'make' from $ROOT" >&2
  exit 1
fi

# Last "Processed in N us" from sim stdout (portable sed; no grep -P).
LAST_MICROS=

run_bench() {
  local label=$1
  shift
  local out
  out=$("$BIN" --seed "$SEED" --num-orders "$NUM_ORDERS" --num-tickers "$NUM_TICKERS" "$@" 2>&1) || true
  if [[ "$VERBOSE" == "1" ]]; then
    echo "=== $label ==="
    printf '%s\n' "$out"
  fi
  LAST_MICROS=$(printf '%s\n' "$out" | sed -n 's/.*Processed in \([0-9][0-9]*\) us.*/\1/p' | tail -n1)
  if [[ ! "$LAST_MICROS" =~ ^[0-9]+$ ]]; then
    if [[ "$VERBOSE" == "1" ]]; then
      echo "error, run make clean and make again" >&2
    fi
    LAST_MICROS=0
  fi
}

# Wall-time speedup vs baseline.
speedup_vs() {
  local base_us=$1
  local run_us=$2
  awk -v b="$base_us" -v r="$run_us" 'BEGIN {
    if (b <= 0 || r <= 0) { print "n/a"; exit }
    printf "%.2f", b / r
  }'
}

if [[ "$VERBOSE" == "1" ]]; then
  printf 'LOB benchmark (seed=%s num-orders=%s num-tickers=%s)\n\n' "$SEED" "$NUM_ORDERS" "$NUM_TICKERS"
fi

run_bench "sequential baseline"
BASE_MICROS=$LAST_MICROS
if [[ "$VERBOSE" == "1" ]]; then
  echo "  (baseline wall time: ${BASE_MICROS} us)"
fi
declare -a SPEED_ROWS
SPEED_ROWS+=("sequential baseline|${BASE_MICROS}|1.00")

run_bench "coarse, single-threaded (lock overhead)" --engine coarse
COARSE_ST_MICROS=$LAST_MICROS
sp_st=$(speedup_vs "$BASE_MICROS" "$COARSE_ST_MICROS")
if [[ "$VERBOSE" == "1" ]]; then
  echo "  -> speedup vs sequential: ${sp_st}x"
  echo
fi
SPEED_ROWS+=("coarse single-threaded|${COARSE_ST_MICROS}|${sp_st}")

for t in 1 2 4 8; do
  run_bench "coarse parallel by ticker, --threads $t" --engine coarse --parallel --threads "$t"
  sp=$(speedup_vs "$BASE_MICROS" "$LAST_MICROS")
  if [[ "$VERBOSE" == "1" ]]; then
    echo "  -> speedup vs sequential: ${sp}x"
    echo
  fi
  SPEED_ROWS+=("coarse parallel threads=${t}|${LAST_MICROS}|${sp}")
done

echo "--- Speedup summary---"
printf '%-42s %12s %12s\n' "configuration" "us" "speedup"
printf '%-42s %12s %12s\n' "------------------------------------------" "------------" "------------"
for row in "${SPEED_ROWS[@]}"; do
  IFS='|' read -r name us sp <<<"$row"
  printf '%-42s %12s %11sx\n' "$name" "$us" "$sp"
done

echo