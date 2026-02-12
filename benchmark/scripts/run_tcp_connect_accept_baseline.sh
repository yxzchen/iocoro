#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
PROJECT_DIR="$(cd -- "${BENCH_DIR}/.." && pwd)"
source "$SCRIPT_DIR/common.sh"
BUILD_DIR="$PROJECT_DIR/build"
ITERATIONS=5
WARMUP=1
SCENARIOS="1000,2000,3000"
BASELINE_FILE=""
REPORT_FILE=""
RUN_TIMEOUT_SEC=120

usage() {
  cat <<'EOF'
Usage: benchmark/scripts/run_tcp_connect_accept_baseline.sh [options]

Options:
  --build-dir DIR      CMake build dir containing benchmark binaries (default: ./build)
  --iterations N       Measured runs per scenario (default: 5)
  --warmup N           Warmup runs per scenario/framework (default: 1)
  --scenarios LIST     Comma-separated connection counts (default: 1000,2000,3000)
  --baseline FILE      Threshold file format: "connections min_ratio"
  --report FILE        Write JSON summary to FILE
  --run-timeout-sec N  Timeout for each benchmark process in seconds (default: 120, 0=disable)
  -h, --help           Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --iterations)
      ITERATIONS="$2"
      shift 2
      ;;
    --warmup)
      WARMUP="$2"
      shift 2
      ;;
    --scenarios)
      SCENARIOS="$2"
      shift 2
      ;;
    --baseline)
      BASELINE_FILE="$2"
      shift 2
      ;;
    --report)
      REPORT_FILE="$2"
      shift 2
      ;;
    --run-timeout-sec)
      RUN_TIMEOUT_SEC="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

bench_require_positive_int "--iterations" "$ITERATIONS"
bench_require_non_negative_int "--warmup" "$WARMUP"
bench_require_non_negative_int "--run-timeout-sec" "$RUN_TIMEOUT_SEC"

BUILD_DIR="$(bench_to_abs_path "$PROJECT_DIR" "$BUILD_DIR")"

IOCORO_BIN="$BUILD_DIR/benchmark/iocoro_tcp_connect_accept"
ASIO_BIN="$BUILD_DIR/benchmark/asio_tcp_connect_accept"

if [[ ! -x "$IOCORO_BIN" ]]; then
  echo "Missing benchmark binary: $IOCORO_BIN" >&2
  exit 1
fi
if [[ ! -x "$ASIO_BIN" ]]; then
  echo "Missing benchmark binary: $ASIO_BIN" >&2
  exit 1
fi

if [[ -n "$BASELINE_FILE" ]]; then
  BASELINE_FILE="$(bench_to_abs_path "$PROJECT_DIR" "$BASELINE_FILE")"
fi

if [[ -n "$BASELINE_FILE" && ! -f "$BASELINE_FILE" ]]; then
  echo "Baseline file not found: $BASELINE_FILE" >&2
  exit 1
fi

if [[ -n "$REPORT_FILE" ]]; then
  REPORT_FILE="$(bench_to_abs_path "$PROJECT_DIR" "$REPORT_FILE")"
fi

check_threshold() {
  local connections="$1"
  if [[ -z "$BASELINE_FILE" ]]; then
    echo ""
    return 0
  fi
  awk -v c="$connections" '
    $1 !~ /^#/ && NF >= 2 && $1 == c { print $2; found = 1; exit }
    END { if (!found) print ""; }
  ' "$BASELINE_FILE"
}

pass_count=0
fail_count=0
table_rows=()
scenario_json=()

echo "Running tcp connect/accept benchmark"
echo "  iocoro: $IOCORO_BIN"
echo "  asio  : $ASIO_BIN"
echo "  scenarios: $SCENARIOS"
echo "  warmup: $WARMUP, measured iterations: $ITERATIONS"
echo "  per-run timeout: ${RUN_TIMEOUT_SEC}s"
if [[ -n "$BASELINE_FILE" ]]; then
  echo "  baseline: $BASELINE_FILE"
fi
echo

IFS=',' read -r -a scenario_counts <<<"$SCENARIOS"
for connections in "${scenario_counts[@]}"; do
  if [[ -z "$connections" ]]; then
    echo "Invalid scenario entry in --scenarios" >&2
    exit 1
  fi

  echo "Scenario connections=$connections"

  for ((i = 0; i < WARMUP; ++i)); do
    bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$IOCORO_BIN" "$connections" >/dev/null
    bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$ASIO_BIN" "$connections" >/dev/null
  done

  iocoro_runs=()
  asio_runs=()

  for ((i = 0; i < ITERATIONS; ++i)); do
    line="$(bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$IOCORO_BIN" "$connections")"
    cps="$(bench_extract_metric "cps" "$line")"
    if [[ -z "$cps" ]]; then
      echo "Failed to parse iocoro cps: $line" >&2
      exit 1
    fi
    iocoro_runs+=("$cps")
  done

  for ((i = 0; i < ITERATIONS; ++i)); do
    line="$(bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$ASIO_BIN" "$connections")"
    cps="$(bench_extract_metric "cps" "$line")"
    if [[ -z "$cps" ]]; then
      echo "Failed to parse asio cps: $line" >&2
      exit 1
    fi
    asio_runs+=("$cps")
  done

  iocoro_median="$(
    printf '%s\n' "${iocoro_runs[@]}" | sort -n | bench_median_from_stdin
  )"
  asio_median="$(
    printf '%s\n' "${asio_runs[@]}" | sort -n | bench_median_from_stdin
  )"
  ratio="$(bench_compute_ratio "$iocoro_median" "$asio_median")"
  min_ratio="$(check_threshold "$connections")"
  pass_label="n/a"
  pass_bool=true

  if [[ -n "$min_ratio" ]]; then
    if awk -v v="$ratio" -v t="$min_ratio" 'BEGIN { exit !(v + 0 >= t + 0) }'; then
      pass_label="PASS"
      pass_count=$((pass_count + 1))
      pass_bool=true
    else
      pass_label="FAIL"
      fail_count=$((fail_count + 1))
      pass_bool=false
    fi
  fi

  table_rows+=("$connections|$iocoro_median|$asio_median|$ratio|${min_ratio:-n/a}|$pass_label")

  iocoro_runs_json="$(printf '%s\n' "${iocoro_runs[@]}" | paste -sd, -)"
  asio_runs_json="$(printf '%s\n' "${asio_runs[@]}" | paste -sd, -)"
  if [[ -n "$min_ratio" ]]; then
    min_ratio_json="$min_ratio"
  else
    min_ratio_json="null"
  fi
  scenario_json+=("{\"connections\":$connections,\"iocoro_cps_runs\":[${iocoro_runs_json}],\"asio_cps_runs\":[${asio_runs_json}],\"iocoro_cps_median\":$iocoro_median,\"asio_cps_median\":$asio_median,\"ratio_vs_asio\":$ratio,\"min_ratio\":$min_ratio_json,\"pass\":$pass_bool}")

  echo "  iocoro median cps: $iocoro_median"
  echo "  asio   median cps: $asio_median"
  echo "  ratio (iocoro/asio): $ratio"
  if [[ -n "$min_ratio" ]]; then
    echo "  threshold: $min_ratio ($pass_label)"
  fi
  echo
done

echo "Summary"
printf '| %-11s | %-16s | %-14s | %-14s | %-10s | %-6s |\n' \
  "connections" "iocoro_cps_median" "asio_cps_median" "ratio_vs_asio" "min_ratio" "gate"
printf '|-%-11s-|-%-16s-|-%-14s-|-%-14s-|-%-10s-|-%-6s-|\n' \
  "-----------" "----------------" "--------------" "--------------" "----------" "------"
for row in "${table_rows[@]}"; do
  IFS='|' read -r c iocoro_m asio_m ratio min gate <<<"$row"
  printf '| %-11s | %-16s | %-14s | %-14s | %-10s | %-6s |\n' \
    "$c" "$iocoro_m" "$asio_m" "$ratio" "$min" "$gate"
done
echo

if [[ -n "$REPORT_FILE" ]]; then
  mkdir -p "$(dirname -- "$REPORT_FILE")"
  timestamp="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  {
    printf '{\n'
    printf '  "schema_version": 1,\n'
    printf '  "timestamp_utc": "%s",\n' "$timestamp"
    printf '  "build_dir": "%s",\n' "$BUILD_DIR"
    printf '  "iterations": %s,\n' "$ITERATIONS"
    printf '  "warmup": %s,\n' "$WARMUP"
    printf '  "scenarios": [\n'
    for ((i = 0; i < ${#scenario_json[@]}; ++i)); do
      suffix=","
      if [[ $i -eq $(( ${#scenario_json[@]} - 1 )) ]]; then
        suffix=""
      fi
      printf '    %s%s\n' "${scenario_json[$i]}" "$suffix"
    done
    printf '  ]\n'
    printf '}\n'
  } >"$REPORT_FILE"
  echo "Wrote report: $REPORT_FILE"
fi

if [[ -n "$BASELINE_FILE" && "$fail_count" -gt 0 ]]; then
  echo
  echo "Performance gate failed: $fail_count scenario(s) below threshold" >&2
  exit 1
fi

if [[ -n "$BASELINE_FILE" ]]; then
  echo "Performance gate passed: $pass_count scenario(s) met thresholds"
fi
