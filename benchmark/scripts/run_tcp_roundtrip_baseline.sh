#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
PROJECT_DIR="$(cd -- "${BENCH_DIR}/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
ITERATIONS=5
WARMUP=1
SCENARIOS="1:5000:64,8:2000:64,32:500:64,8:1000:1024,32:200:4096"
BASELINE_FILE=""
REPORT_FILE=""
RUN_TIMEOUT_SEC=60

usage() {
  cat <<'EOF'
Usage: benchmark/scripts/run_tcp_roundtrip_baseline.sh [options]

Options:
  --build-dir DIR      CMake build dir containing benchmark binaries (default: ./build)
  --iterations N       Measured runs per scenario (default: 5)
  --warmup N           Warmup runs per scenario/framework (default: 1)
  --scenarios LIST     Comma-separated sessions:msgs:msg_bytes tuples
                       (default: 1:5000:64,8:2000:64,32:500:64,8:1000:1024,32:200:4096)
  --baseline FILE      Threshold file:
                       - legacy format: "sessions msgs min_ratio" (msg_bytes=13)
                       - new format   : "sessions msgs msg_bytes min_ratio"
  --report FILE        Write JSON summary to FILE
  --run-timeout-sec N  Timeout for each benchmark process in seconds (default: 60, 0=disable)
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

if ! [[ "$ITERATIONS" =~ ^[0-9]+$ ]] || [[ "$ITERATIONS" -le 0 ]]; then
  echo "--iterations must be a positive integer" >&2
  exit 1
fi

if ! [[ "$WARMUP" =~ ^[0-9]+$ ]]; then
  echo "--warmup must be a non-negative integer" >&2
  exit 1
fi

if ! [[ "$RUN_TIMEOUT_SEC" =~ ^[0-9]+$ ]]; then
  echo "--run-timeout-sec must be a non-negative integer" >&2
  exit 1
fi

if [[ "$BUILD_DIR" != /* ]]; then
  BUILD_DIR="$PROJECT_DIR/$BUILD_DIR"
fi

IOCORO_BIN="$BUILD_DIR/benchmark/iocoro_tcp_roundtrip"
ASIO_BIN="$BUILD_DIR/benchmark/asio_tcp_roundtrip"

if [[ ! -x "$IOCORO_BIN" ]]; then
  echo "Missing benchmark binary: $IOCORO_BIN" >&2
  exit 1
fi
if [[ ! -x "$ASIO_BIN" ]]; then
  echo "Missing benchmark binary: $ASIO_BIN" >&2
  exit 1
fi

if [[ -n "$BASELINE_FILE" && "$BASELINE_FILE" != /* ]]; then
  BASELINE_FILE="$PROJECT_DIR/$BASELINE_FILE"
fi

if [[ -n "$BASELINE_FILE" && ! -f "$BASELINE_FILE" ]]; then
  echo "Baseline file not found: $BASELINE_FILE" >&2
  exit 1
fi

if [[ -n "$REPORT_FILE" && "$REPORT_FILE" != /* ]]; then
  REPORT_FILE="$PROJECT_DIR/$REPORT_FILE"
fi

run_bench_cmd() {
  local out
  local code=0
  if [[ "$RUN_TIMEOUT_SEC" -gt 0 ]] && command -v timeout >/dev/null 2>&1; then
    set +e
    out="$(timeout --foreground "${RUN_TIMEOUT_SEC}s" "$@" 2>&1)"
    code=$?
    set -e
    if [[ $code -ne 0 ]]; then
      if [[ $code -eq 124 ]]; then
        echo "Benchmark timed out after ${RUN_TIMEOUT_SEC}s: $*" >&2
      else
        echo "Benchmark command failed ($code): $*" >&2
        echo "$out" >&2
      fi
      exit 1
    fi
  else
    set +e
    out="$("$@" 2>&1)"
    code=$?
    set -e
    if [[ $code -ne 0 ]]; then
      echo "Benchmark command failed ($code): $*" >&2
      echo "$out" >&2
      exit 1
    fi
  fi
  printf '%s\n' "$out"
}

extract_rps() {
  sed -nE 's/.* rps=([0-9]+([.][0-9]+)?).*/\1/p' <<<"$1"
}

median_from_stdin() {
  awk '
    { vals[++n] = $1; }
    END {
      if (n == 0) { print "0"; exit; }
      mid = int((n + 1) / 2);
      if (n % 2 == 1) {
        printf "%.2f\n", vals[mid];
      } else {
        printf "%.2f\n", (vals[mid] + vals[mid + 1]) / 2.0;
      }
    }
  '
}

compute_ratio() {
  awk -v i="$1" -v a="$2" 'BEGIN { if (a <= 0) { print "0.00"; } else { printf "%.4f\n", i / a; } }'
}

check_threshold() {
  local sessions="$1"
  local msgs="$2"
  local msg_bytes="$3"
  if [[ -z "$BASELINE_FILE" ]]; then
    echo ""
    return 0
  fi
  awk -v s="$sessions" -v m="$msgs" -v b="$msg_bytes" '
    $1 ~ /^#/ { next }
    NF >= 4 && $1 == s && $2 == m && $3 == b { print $4; found = 1; exit }
    NF >= 3 && $1 == s && $2 == m && b == 13 { print $3; found = 1; exit }
    END { if (!found) print ""; }
  ' "$BASELINE_FILE"
}

pass_count=0
fail_count=0
table_rows=()
scenario_json=()

echo "Running tcp roundtrip benchmark"
echo "  iocoro: $IOCORO_BIN"
echo "  asio  : $ASIO_BIN"
echo "  scenarios: $SCENARIOS"
echo "  warmup: $WARMUP, measured iterations: $ITERATIONS"
echo "  per-run timeout: ${RUN_TIMEOUT_SEC}s"
if [[ -n "$BASELINE_FILE" ]]; then
  echo "  baseline: $BASELINE_FILE"
fi
echo

IFS=',' read -r -a scenario_pairs <<< "$SCENARIOS"
for pair in "${scenario_pairs[@]}"; do
  IFS=':' read -r sessions msgs msg_bytes <<< "$pair"
  if [[ -z "${sessions:-}" || -z "${msgs:-}" ]]; then
    echo "Invalid scenario: $pair (expected sessions:msgs[:msg_bytes])" >&2
    exit 1
  fi
  if [[ -z "${msg_bytes:-}" ]]; then
    msg_bytes=13
  fi

  echo "Scenario sessions=$sessions msgs=$msgs msg_bytes=$msg_bytes"

  for ((i = 0; i < WARMUP; ++i)); do
    run_bench_cmd "$IOCORO_BIN" "$sessions" "$msgs" "$msg_bytes" >/dev/null
    run_bench_cmd "$ASIO_BIN" "$sessions" "$msgs" "$msg_bytes" >/dev/null
  done

  iocoro_runs=()
  asio_runs=()

  for ((i = 0; i < ITERATIONS; ++i)); do
    line="$(run_bench_cmd "$IOCORO_BIN" "$sessions" "$msgs" "$msg_bytes")"
    rps="$(extract_rps "$line")"
    if [[ -z "$rps" ]]; then
      echo "Failed to parse iocoro rps: $line" >&2
      exit 1
    fi
    iocoro_runs+=("$rps")
  done

  for ((i = 0; i < ITERATIONS; ++i)); do
    line="$(run_bench_cmd "$ASIO_BIN" "$sessions" "$msgs" "$msg_bytes")"
    rps="$(extract_rps "$line")"
    if [[ -z "$rps" ]]; then
      echo "Failed to parse asio rps: $line" >&2
      exit 1
    fi
    asio_runs+=("$rps")
  done

  iocoro_median="$(
    printf '%s\n' "${iocoro_runs[@]}" | sort -n | median_from_stdin
  )"
  asio_median="$(
    printf '%s\n' "${asio_runs[@]}" | sort -n | median_from_stdin
  )"
  ratio="$(compute_ratio "$iocoro_median" "$asio_median")"
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

  table_rows+=("$sessions|$msgs|$msg_bytes|$iocoro_median|$asio_median|$ratio|${min_ratio:-n/a}|$pass_label")

  iocoro_runs_json="$(printf '%s\n' "${iocoro_runs[@]}" | paste -sd, -)"
  asio_runs_json="$(printf '%s\n' "${asio_runs[@]}" | paste -sd, -)"
  if [[ -n "$min_ratio" ]]; then
    min_ratio_json="$min_ratio"
  else
    min_ratio_json="null"
  fi
  scenario_json+=("{\"sessions\":$sessions,\"msgs\":$msgs,\"msg_bytes\":$msg_bytes,\"iocoro_rps_runs\":[${iocoro_runs_json}],\"asio_rps_runs\":[${asio_runs_json}],\"iocoro_rps_median\":$iocoro_median,\"asio_rps_median\":$asio_median,\"ratio_vs_asio\":$ratio,\"min_ratio\":$min_ratio_json,\"pass\":$pass_bool}")

  echo "  iocoro median rps: $iocoro_median"
  echo "  asio   median rps: $asio_median"
  echo "  ratio (iocoro/asio): $ratio"
  if [[ -n "$min_ratio" ]]; then
    echo "  threshold: $min_ratio ($pass_label)"
  fi
  echo
done

echo "Summary"
printf '| %-8s | %-6s | %-9s | %-16s | %-14s | %-14s | %-10s | %-6s |\n' \
  "sessions" "msgs" "msg_bytes" "iocoro_rps_median" "asio_rps_median" "ratio_vs_asio" "min_ratio" "gate"
printf '|-%-8s-|-%-6s-|-%-9s-|-%-16s-|-%-14s-|-%-14s-|-%-10s-|-%-6s-|\n' \
  "--------" "------" "---------" "----------------" "--------------" "--------------" "----------" "------"
for row in "${table_rows[@]}"; do
  IFS='|' read -r s m b iocoro_m asio_m ratio min gate <<<"$row"
  printf '| %-8s | %-6s | %-9s | %-16s | %-14s | %-14s | %-10s | %-6s |\n' \
    "$s" "$m" "$b" "$iocoro_m" "$asio_m" "$ratio" "$min" "$gate"
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
