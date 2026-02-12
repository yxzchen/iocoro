#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
PROJECT_DIR="$(cd -- "${BENCH_DIR}/.." && pwd)"
source "$SCRIPT_DIR/common.sh"
BUILD_DIR="$PROJECT_DIR/build"
ITERATIONS=5
WARMUP=1
SCENARIOS="1:5000:64,8:1000:64,8:500:1024"
BASELINE_FILE=""
REPORT_FILE=""
RUN_TIMEOUT_SEC=90

usage() {
  cat <<'EOF'
Usage: benchmark/scripts/run_tcp_latency_baseline.sh [options]

Options:
  --build-dir DIR      CMake build dir containing benchmark binaries (default: ./build)
  --iterations N       Measured runs per scenario (default: 5)
  --warmup N           Warmup runs per scenario/framework (default: 1)
  --scenarios LIST     Comma-separated sessions:msgs:msg_bytes tuples
                       (default: 1:5000:64,8:1000:64,8:500:1024)
  --baseline FILE      Threshold file format: "sessions msgs msg_bytes min_ratio"
                       where ratio is asio_p95 / iocoro_p95 (higher is better).
  --report FILE        Write JSON summary to FILE
  --run-timeout-sec N  Timeout for each benchmark process in seconds (default: 90, 0=disable)
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

IOCORO_BIN="$BUILD_DIR/benchmark/iocoro_tcp_latency"
ASIO_BIN="$BUILD_DIR/benchmark/asio_tcp_latency"

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
  local sessions="$1"
  local msgs="$2"
  local msg_bytes="$3"
  if [[ -z "$BASELINE_FILE" ]]; then
    echo ""
    return 0
  fi
  awk -v s="$sessions" -v m="$msgs" -v b="$msg_bytes" '
    $1 !~ /^#/ && NF >= 4 && $1 == s && $2 == m && $3 == b { print $4; found = 1; exit }
    END { if (!found) print ""; }
  ' "$BASELINE_FILE"
}

pass_count=0
fail_count=0
table_rows=()
scenario_json=()

echo "Running tcp latency benchmark"
echo "  iocoro: $IOCORO_BIN"
echo "  asio  : $ASIO_BIN"
echo "  scenarios: $SCENARIOS"
echo "  warmup: $WARMUP, measured iterations: $ITERATIONS"
echo "  per-run timeout: ${RUN_TIMEOUT_SEC}s"
if [[ -n "$BASELINE_FILE" ]]; then
  echo "  baseline: $BASELINE_FILE"
fi
echo

IFS=',' read -r -a scenario_pairs <<<"$SCENARIOS"
for pair in "${scenario_pairs[@]}"; do
  IFS=':' read -r sessions msgs msg_bytes <<<"$pair"
  if [[ -z "${sessions:-}" || -z "${msgs:-}" || -z "${msg_bytes:-}" ]]; then
    echo "Invalid scenario: $pair (expected sessions:msgs:msg_bytes)" >&2
    exit 1
  fi

  echo "Scenario sessions=$sessions msgs=$msgs msg_bytes=$msg_bytes"

  for ((i = 0; i < WARMUP; ++i)); do
    bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$IOCORO_BIN" "$sessions" "$msgs" "$msg_bytes" >/dev/null
    bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$ASIO_BIN" "$sessions" "$msgs" "$msg_bytes" >/dev/null
  done

  iocoro_p50_runs=()
  iocoro_p95_runs=()
  iocoro_p99_runs=()
  asio_p50_runs=()
  asio_p95_runs=()
  asio_p99_runs=()

  for ((i = 0; i < ITERATIONS; ++i)); do
    line="$(bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$IOCORO_BIN" "$sessions" "$msgs" "$msg_bytes")"
    p50="$(bench_extract_metric "p50_us" "$line")"
    p95="$(bench_extract_metric "p95_us" "$line")"
    p99="$(bench_extract_metric "p99_us" "$line")"
    if [[ -z "$p50" || -z "$p95" || -z "$p99" ]]; then
      echo "Failed to parse iocoro latency metrics: $line" >&2
      exit 1
    fi
    iocoro_p50_runs+=("$p50")
    iocoro_p95_runs+=("$p95")
    iocoro_p99_runs+=("$p99")
  done

  for ((i = 0; i < ITERATIONS; ++i)); do
    line="$(bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$ASIO_BIN" "$sessions" "$msgs" "$msg_bytes")"
    p50="$(bench_extract_metric "p50_us" "$line")"
    p95="$(bench_extract_metric "p95_us" "$line")"
    p99="$(bench_extract_metric "p99_us" "$line")"
    if [[ -z "$p50" || -z "$p95" || -z "$p99" ]]; then
      echo "Failed to parse asio latency metrics: $line" >&2
      exit 1
    fi
    asio_p50_runs+=("$p50")
    asio_p95_runs+=("$p95")
    asio_p99_runs+=("$p99")
  done

  iocoro_p50_median="$(
    printf '%s\n' "${iocoro_p50_runs[@]}" | sort -n | bench_median_from_stdin
  )"
  iocoro_p95_median="$(
    printf '%s\n' "${iocoro_p95_runs[@]}" | sort -n | bench_median_from_stdin
  )"
  iocoro_p99_median="$(
    printf '%s\n' "${iocoro_p99_runs[@]}" | sort -n | bench_median_from_stdin
  )"
  asio_p50_median="$(
    printf '%s\n' "${asio_p50_runs[@]}" | sort -n | bench_median_from_stdin
  )"
  asio_p95_median="$(
    printf '%s\n' "${asio_p95_runs[@]}" | sort -n | bench_median_from_stdin
  )"
  asio_p99_median="$(
    printf '%s\n' "${asio_p99_runs[@]}" | sort -n | bench_median_from_stdin
  )"

  ratio="$(bench_compute_ratio "$asio_p95_median" "$iocoro_p95_median")"
  min_ratio="$(check_threshold "$sessions" "$msgs" "$msg_bytes")"
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

  table_rows+=("$sessions|$msgs|$msg_bytes|$iocoro_p95_median|$asio_p95_median|$ratio|${min_ratio:-n/a}|$pass_label")

  iocoro_p50_json="$(printf '%s\n' "${iocoro_p50_runs[@]}" | paste -sd, -)"
  iocoro_p95_json="$(printf '%s\n' "${iocoro_p95_runs[@]}" | paste -sd, -)"
  iocoro_p99_json="$(printf '%s\n' "${iocoro_p99_runs[@]}" | paste -sd, -)"
  asio_p50_json="$(printf '%s\n' "${asio_p50_runs[@]}" | paste -sd, -)"
  asio_p95_json="$(printf '%s\n' "${asio_p95_runs[@]}" | paste -sd, -)"
  asio_p99_json="$(printf '%s\n' "${asio_p99_runs[@]}" | paste -sd, -)"

  if [[ -n "$min_ratio" ]]; then
    min_ratio_json="$min_ratio"
  else
    min_ratio_json="null"
  fi

  scenario_json+=("{\"sessions\":$sessions,\"msgs\":$msgs,\"msg_bytes\":$msg_bytes,\"iocoro_p50_us_runs\":[${iocoro_p50_json}],\"iocoro_p95_us_runs\":[${iocoro_p95_json}],\"iocoro_p99_us_runs\":[${iocoro_p99_json}],\"asio_p50_us_runs\":[${asio_p50_json}],\"asio_p95_us_runs\":[${asio_p95_json}],\"asio_p99_us_runs\":[${asio_p99_json}],\"iocoro_p50_us_median\":$iocoro_p50_median,\"iocoro_p95_us_median\":$iocoro_p95_median,\"iocoro_p99_us_median\":$iocoro_p99_median,\"asio_p50_us_median\":$asio_p50_median,\"asio_p95_us_median\":$asio_p95_median,\"asio_p99_us_median\":$asio_p99_median,\"ratio_vs_asio_p95\":$ratio,\"min_ratio\":$min_ratio_json,\"pass\":$pass_bool}")

  echo "  iocoro p50/p95/p99 us: $iocoro_p50_median / $iocoro_p95_median / $iocoro_p99_median"
  echo "  asio   p50/p95/p99 us: $asio_p50_median / $asio_p95_median / $asio_p99_median"
  echo "  ratio (asio_p95 / iocoro_p95): $ratio"
  if [[ -n "$min_ratio" ]]; then
    echo "  threshold: $min_ratio ($pass_label)"
  fi
  echo
done

echo "Summary"
printf '| %-8s | %-6s | %-9s | %-16s | %-14s | %-14s | %-10s | %-6s |\n' \
  "sessions" "msgs" "msg_bytes" "iocoro_p95_us" "asio_p95_us" "ratio_vs_asio" "min_ratio" "gate"
printf '|-%-8s-|-%-6s-|-%-9s-|-%-16s-|-%-14s-|-%-14s-|-%-10s-|-%-6s-|\n' \
  "--------" "------" "---------" "----------------" "--------------" "--------------" "----------" "------"
for row in "${table_rows[@]}"; do
  IFS='|' read -r s m b iocoro_p95 asio_p95 ratio min gate <<<"$row"
  printf '| %-8s | %-6s | %-9s | %-16s | %-14s | %-14s | %-10s | %-6s |\n' \
    "$s" "$m" "$b" "$iocoro_p95" "$asio_p95" "$ratio" "$min" "$gate"
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
