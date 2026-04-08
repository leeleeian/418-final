#!/usr/bin/env bash
# Throughput-oriented runs for Week 2. Latency would need per-message timing in sim.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/build/sim"
SEED="${SEED:-42}"
NUM_ORDERS="${NUM_ORDERS:-500000}"

if [[ ! -x "$BIN" ]]; then
  echo "error: $BIN not found; run 'make' from $ROOT" >&2
  exit 1
fi

run() {
  local label=$1
  shift
  echo "=== $label ==="
  "$BIN" --seed "$SEED" --num-orders "$NUM_ORDERS" "$@"
}

echo "LOB benchmark (seed=$SEED num-orders=$NUM_ORDERS)"
echo

run "sequential baseline"
echo

run "coarse, single-threaded (lock overhead)" --engine coarse
echo

for t in 1 2 4 8; do
  run "coarse parallel by ticker, --threads $t" --engine coarse --parallel --threads "$t"
  echo
done

echo "Tip: for golden regression use default sim (sequential) or 'make verify'."
