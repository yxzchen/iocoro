#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
PROJECT_DIR="$(cd -- "${BENCH_DIR}/.." && pwd)"
source "$SCRIPT_DIR/common.sh"

SUITE_NAME=""
USAGE_NAME="benchmark/scripts/run_ratio_baseline_suite.sh"
SCENARIO_FIELDS_CSV=""
SCENARIO_FORMAT=""
SCENARIOS_DEFAULT=""
BASELINE_DESCRIPTION="Threshold file format: \"<scenario...> min_ratio\""
IOCORO_TARGET=""
ASIO_TARGET=""
METRIC_NAME=""
RATIO_MODE="direct"
RATIO_FIELD="ratio_vs_asio"
RUN_TIMEOUT_DEFAULT=120

BUILD_DIR="$PROJECT_DIR/build"
ITERATIONS=5
WARMUP=1
SCENARIOS=""
BASELINE_FILE=""
REPORT_FILE=""
RUN_TIMEOUT_SEC=""

usage() {
  cat <<EOF2
Usage: ${USAGE_NAME} [options]

Options:
  --build-dir DIR      CMake build dir containing benchmark binaries (default: ./build)
  --iterations N       Measured runs per scenario (default: 5)
  --warmup N           Warmup runs per scenario/framework (default: 1)
  --scenarios LIST     Comma-separated ${SCENARIO_FORMAT:-scenario tuples}
                       (default: ${SCENARIOS_DEFAULT:-none})
  --baseline FILE      ${BASELINE_DESCRIPTION}
  --report FILE        Write JSON summary to FILE
  --run-timeout-sec N  Timeout for each benchmark process in seconds (default: ${RUN_TIMEOUT_DEFAULT}, 0=disable)
  -h, --help           Show this help
EOF2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --suite-name)
      SUITE_NAME="$2"
      shift 2
      ;;
    --usage-name)
      USAGE_NAME="$2"
      shift 2
      ;;
    --scenario-fields)
      SCENARIO_FIELDS_CSV="$2"
      shift 2
      ;;
    --scenario-format)
      SCENARIO_FORMAT="$2"
      shift 2
      ;;
    --scenarios-default)
      SCENARIOS_DEFAULT="$2"
      shift 2
      ;;
    --baseline-description)
      BASELINE_DESCRIPTION="$2"
      shift 2
      ;;
    --iocoro-target)
      IOCORO_TARGET="$2"
      shift 2
      ;;
    --asio-target)
      ASIO_TARGET="$2"
      shift 2
      ;;
    --metric-name)
      METRIC_NAME="$2"
      shift 2
      ;;
    --ratio-mode)
      RATIO_MODE="$2"
      shift 2
      ;;
    --ratio-field)
      RATIO_FIELD="$2"
      shift 2
      ;;
    --run-timeout-default)
      RUN_TIMEOUT_DEFAULT="$2"
      shift 2
      ;;
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

if [[ -z "$SUITE_NAME" || -z "$SCENARIO_FIELDS_CSV" || -z "$IOCORO_TARGET" || -z "$ASIO_TARGET" || -z "$METRIC_NAME" ]]; then
  echo "Missing required suite configuration in wrapper script" >&2
  exit 1
fi

if [[ -z "$SCENARIOS" ]]; then
  SCENARIOS="$SCENARIOS_DEFAULT"
fi
if [[ -z "$SCENARIOS" ]]; then
  echo "--scenarios is required" >&2
  exit 1
fi

if [[ -z "$RUN_TIMEOUT_SEC" ]]; then
  RUN_TIMEOUT_SEC="$RUN_TIMEOUT_DEFAULT"
fi

if [[ "$RATIO_MODE" != "direct" && "$RATIO_MODE" != "inverse" ]]; then
  echo "--ratio-mode must be one of: direct, inverse" >&2
  exit 1
fi

bench_require_positive_int "--iterations" "$ITERATIONS"
bench_require_non_negative_int "--warmup" "$WARMUP"
bench_require_non_negative_int "--run-timeout-sec" "$RUN_TIMEOUT_SEC"

BUILD_DIR="$(bench_to_abs_path "$PROJECT_DIR" "$BUILD_DIR")"
IOCORO_BIN="$BUILD_DIR/benchmark/$IOCORO_TARGET"
ASIO_BIN="$BUILD_DIR/benchmark/$ASIO_TARGET"

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
  if [[ ! -f "$BASELINE_FILE" ]]; then
    echo "Baseline file not found: $BASELINE_FILE" >&2
    exit 1
  fi
fi

if [[ -n "$REPORT_FILE" ]]; then
  REPORT_FILE="$(bench_to_abs_path "$PROJECT_DIR" "$REPORT_FILE")"
fi

IFS=',' read -r -a SCENARIO_FIELDS <<<"$SCENARIO_FIELDS_CSV"
SCENARIO_ARITY=${#SCENARIO_FIELDS[@]}
if [[ "$SCENARIO_ARITY" -le 0 ]]; then
  echo "--scenario-fields must not be empty" >&2
  exit 1
fi

check_threshold() {
  if [[ -z "$BASELINE_FILE" ]]; then
    echo ""
    return 0
  fi

  local key
  key="$(IFS='|'; echo "$*")"
  awk -v arity="$SCENARIO_ARITY" -v key="$key" '
    BEGIN { split(key, want, /\|/); }
    $1 ~ /^#/ { next }
    NF < arity + 1 { next }
    {
      ok = 1
      for (i = 1; i <= arity; ++i) {
        if ($i != want[i]) {
          ok = 0
          break
        }
      }
      if (ok) {
        print $(arity + 1)
        found = 1
        exit
      }
    }
    END { if (!found) print "" }
  ' "$BASELINE_FILE"
}

pass_count=0
fail_count=0
table_rows=()
scenario_json=()

ratio_expr="iocoro/asio"
if [[ "$RATIO_MODE" == "inverse" ]]; then
  ratio_expr="asio/iocoro"
fi

echo "Running $SUITE_NAME"
echo "  iocoro: $IOCORO_BIN"
echo "  asio  : $ASIO_BIN"
echo "  scenarios: $SCENARIOS"
echo "  warmup: $WARMUP, measured iterations: $ITERATIONS"
echo "  per-run timeout: ${RUN_TIMEOUT_SEC}s"
if [[ -n "$BASELINE_FILE" ]]; then
  echo "  baseline: $BASELINE_FILE"
fi
echo

IFS=',' read -r -a SCENARIO_ITEMS <<<"$SCENARIOS"
for item in "${SCENARIO_ITEMS[@]}"; do
  IFS=':' read -r -a VALUES <<<"$item"
  if [[ ${#VALUES[@]} -ne $SCENARIO_ARITY ]]; then
    echo "Invalid scenario: $item (expected $SCENARIO_FORMAT)" >&2
    exit 1
  fi

  for value in "${VALUES[@]}"; do
    if [[ -z "$value" ]]; then
      echo "Invalid scenario: $item (empty field)" >&2
      exit 1
    fi
  done

  scenario_desc=()
  for ((idx = 0; idx < SCENARIO_ARITY; ++idx)); do
    scenario_desc+=("${SCENARIO_FIELDS[$idx]}=${VALUES[$idx]}")
  done
  echo "Scenario ${scenario_desc[*]}"

  for ((i = 0; i < WARMUP; ++i)); do
    bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$IOCORO_BIN" "${VALUES[@]}" >/dev/null
    bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$ASIO_BIN" "${VALUES[@]}" >/dev/null
  done

  iocoro_runs=()
  asio_runs=()

  for ((i = 0; i < ITERATIONS; ++i)); do
    line="$(bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$IOCORO_BIN" "${VALUES[@]}")"
    metric_value="$(bench_extract_metric "$METRIC_NAME" "$line")"
    if [[ -z "$metric_value" ]]; then
      echo "Failed to parse iocoro $METRIC_NAME: $line" >&2
      exit 1
    fi
    iocoro_runs+=("$metric_value")
  done

  for ((i = 0; i < ITERATIONS; ++i)); do
    line="$(bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$ASIO_BIN" "${VALUES[@]}")"
    metric_value="$(bench_extract_metric "$METRIC_NAME" "$line")"
    if [[ -z "$metric_value" ]]; then
      echo "Failed to parse asio $METRIC_NAME: $line" >&2
      exit 1
    fi
    asio_runs+=("$metric_value")
  done

  iocoro_median="$({ printf '%s\n' "${iocoro_runs[@]}" | sort -n | bench_median_from_stdin; })"
  asio_median="$({ printf '%s\n' "${asio_runs[@]}" | sort -n | bench_median_from_stdin; })"

  if [[ "$RATIO_MODE" == "direct" ]]; then
    ratio="$(bench_compute_ratio "$iocoro_median" "$asio_median")"
  else
    ratio="$(bench_compute_ratio "$asio_median" "$iocoro_median")"
  fi

  min_ratio="$(check_threshold "${VALUES[@]}")"
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

  row_prefix="$(IFS='|'; echo "${VALUES[*]}")"
  table_rows+=("${row_prefix}|${iocoro_median}|${asio_median}|${ratio}|${min_ratio:-n/a}|${pass_label}")

  scenario_prefix=""
  for ((idx = 0; idx < SCENARIO_ARITY; ++idx)); do
    scenario_prefix+="\"${SCENARIO_FIELDS[$idx]}\":${VALUES[$idx]},"
  done

  iocoro_runs_json="$(printf '%s\n' "${iocoro_runs[@]}" | paste -sd, -)"
  asio_runs_json="$(printf '%s\n' "${asio_runs[@]}" | paste -sd, -)"
  if [[ -n "$min_ratio" ]]; then
    min_ratio_json="$min_ratio"
  else
    min_ratio_json="null"
  fi

  scenario_json+=("{${scenario_prefix}\"iocoro_${METRIC_NAME}_runs\":[${iocoro_runs_json}],\"asio_${METRIC_NAME}_runs\":[${asio_runs_json}],\"iocoro_${METRIC_NAME}_median\":${iocoro_median},\"asio_${METRIC_NAME}_median\":${asio_median},\"${RATIO_FIELD}\":${ratio},\"min_ratio\":${min_ratio_json},\"pass\":${pass_bool}}")

  echo "  iocoro median $METRIC_NAME: $iocoro_median"
  echo "  asio   median $METRIC_NAME: $asio_median"
  echo "  ratio ($ratio_expr): $ratio"
  if [[ -n "$min_ratio" ]]; then
    echo "  threshold: $min_ratio ($pass_label)"
  fi
  echo
done

echo "Summary"
header=("${SCENARIO_FIELDS[@]}" "iocoro_${METRIC_NAME}_median" "asio_${METRIC_NAME}_median" "$RATIO_FIELD" "min_ratio" "gate")
printf '|'
for col in "${header[@]}"; do
  printf ' %s |' "$col"
done
printf '\n|'
for _ in "${header[@]}"; do
  printf ' --- |'
done
printf '\n'
for row in "${table_rows[@]}"; do
  IFS='|' read -r -a cols <<<"$row"
  printf '|'
  for col in "${cols[@]}"; do
    printf ' %s |' "$col"
  done
  printf '\n'
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
