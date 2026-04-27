#!/usr/bin/env bash
# Analyze profile CSV and generate summary statistics
#
# Usage: ./scripts/analyze_profile.sh [csv_file]

set -euo pipefail

CSV_FILE="${1:-results/profile_comprehensive.csv}"

if [[ ! -f "$CSV_FILE" ]]; then
  echo "error: $CSV_FILE not found" >&2
  exit 1
fi

echo "Profile Analysis Report"
echo "================================================================"
echo "CSV: $CSV_FILE"
echo ""

# Overall statistics
echo "OVERALL STATISTICS"
echo "================================================================"
echo ""

# Count tests
total_rows=$(tail -n +2 "$CSV_FILE" | wc -l)
coarse_count=$(grep ",coarse," "$CSV_FILE" | wc -l)
fine_count=$(grep ",fine," "$CSV_FILE" | wc -l)

echo "Total test runs: $total_rows"
echo "  Coarse-grained: $coarse_count"
echo "  Fine-grained: $fine_count"
echo ""

# Speedup analysis
echo "SPEEDUP ANALYSIS (coarse_time / fine_time)"
echo "================================================================"
echo ""

declare -A speedups
declare -A counts

# Calculate speedups by workload
for workload in balanced crossing resting; do
  echo "$workload workload:"

  # All configs speedup
  total_fine_time=0
  total_coarse_time=0
  config_count=0

  while IFS=',' read -r w o t th e wt cycles insn ipc misses l1 mr; do
    if [[ "$e" == "fine" && "$w" == "$workload" ]]; then
      total_fine_time=$(awk "BEGIN {print $total_fine_time + $wt}")
      config_count=$((config_count + 1))
    elif [[ "$e" == "coarse" && "$w" == "$workload" ]]; then
      total_coarse_time=$(awk "BEGIN {print $total_coarse_time + $wt}")
    fi
  done < <(tail -n +2 "$CSV_FILE")

  overall_speedup=$(awk "BEGIN {if ($total_fine_time > 0) printf \"%.2f\", $total_coarse_time / $total_fine_time; else print \"n/a\"}")
  echo "  Overall speedup: $overall_speedup (configs: $config_count)"

  # By order count
  for orders in 100000 500000 5000000; do
    fine_sum=0
    coarse_sum=0
    count=0

    while IFS=',' read -r w o t th e wt cycles insn ipc misses l1 mr; do
      if [[ "$w" == "$workload" && "$o" == "$orders" ]]; then
        if [[ "$e" == "fine" ]]; then
          fine_sum=$(awk "BEGIN {print $fine_sum + $wt}")
        elif [[ "$e" == "coarse" ]]; then
          coarse_sum=$(awk "BEGIN {print $coarse_sum + $wt}")
        fi
        count=$((count + 1))
      fi
    done < <(tail -n +2 "$CSV_FILE")

    if [[ $count -gt 0 ]]; then
      speedup=$(awk "BEGIN {if ($fine_sum > 0) printf \"%.2f\", $coarse_sum / $fine_sum; else print \"n/a\"}")
      label=$(printf '%s' "$orders" | sed 's/000000/M/; s/000$/k/')
      echo "    $label: $speedup"
    fi
  done

  echo ""
done

# Cache behavior analysis
echo ""
echo "CACHE ANALYSIS (L1 miss rate %)"
echo "================================================================"
echo ""

for workload in balanced crossing resting; do
  echo "$workload workload:"

  declare -a fine_rates
  declare -a coarse_rates

  while IFS=',' read -r w o t th e wt cycles insn ipc misses l1 mr; do
    if [[ "$w" == "$workload" && "$th" == "8" ]]; then
      if [[ "$e" == "fine" && ! -z "$mr" && "$mr" != "n/a" ]]; then
        fine_rates+=("$mr")
      elif [[ "$e" == "coarse" && ! -z "$mr" && "$mr" != "n/a" ]]; then
        coarse_rates+=("$mr")
      fi
    fi
  done < <(tail -n +2 "$CSV_FILE")

  if [[ ${#fine_rates[@]} -gt 0 ]]; then
    fine_avg=$(printf '%s\n' "${fine_rates[@]}" | awk '{s+=$1} END {print s/NR}')
    coarse_avg=$(printf '%s\n' "${coarse_rates[@]}" | awk '{s+=$1} END {print s/NR}')
    echo "  Fine avg L1 miss rate: $(printf '%.2f' $fine_avg)%"
    echo "  Coarse avg L1 miss rate: $(printf '%.2f' $coarse_avg)%"
  fi

  echo ""
done

# IPC analysis
echo ""
echo "IPC ANALYSIS (Instructions per Cycle)"
echo "================================================================"
echo ""

for workload in balanced crossing resting; do
  echo "$workload workload:"

  declare -a fine_ipcs
  declare -a coarse_ipcs

  while IFS=',' read -r w o t th e wt cycles insn ipc misses l1 mr; do
    if [[ "$w" == "$workload" && "$th" == "8" ]]; then
      if [[ "$e" == "fine" && ! -z "$ipc" && "$ipc" != "n/a" ]]; then
        fine_ipcs+=("$ipc")
      elif [[ "$e" == "coarse" && ! -z "$ipc" && "$ipc" != "n/a" ]]; then
        coarse_ipcs+=("$ipc")
      fi
    fi
  done < <(tail -n +2 "$CSV_FILE")

  if [[ ${#fine_ipcs[@]} -gt 0 ]]; then
    fine_avg=$(printf '%s\n' "${fine_ipcs[@]}" | awk '{s+=$1} END {print s/NR}')
    coarse_avg=$(printf '%s\n' "${coarse_ipcs[@]}" | awk '{s+=$1} END {print s/NR}')
    echo "  Fine avg IPC: $(printf '%.3f' $fine_avg)"
    echo "  Coarse avg IPC: $(printf '%.3f' $coarse_avg)"
  fi

  echo ""
done

echo "================================================================"
echo "Done."
