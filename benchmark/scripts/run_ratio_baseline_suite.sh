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
METRIC_NAMES_CSV=""
PRIMARY_METRIC=""
RATIO_MODE="direct"
RATIO_FIELD="ratio_vs_asio"
RATIO_LABEL=""
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
    --metric-names)
      METRIC_NAMES_CSV="$2"
      shift 2
      ;;
    --primary-metric)
      PRIMARY_METRIC="$2"
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
    --ratio-label)
      RATIO_LABEL="$2"
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

if [[ -z "$SUITE_NAME" || -z "$SCENARIO_FIELDS_CSV" || -z "$IOCORO_TARGET" || -z "$ASIO_TARGET" ]]; then
  echo "Missing required suite configuration in wrapper script" >&2
  exit 1
fi

if [[ -z "$METRIC_NAMES_CSV" ]]; then
  METRIC_NAMES_CSV="$METRIC_NAME"
fi
if [[ -z "$METRIC_NAMES_CSV" ]]; then
  echo "Missing metric configuration: use --metric-name or --metric-names" >&2
  exit 1
fi

IFS=',' read -r -a METRIC_NAMES <<<"$METRIC_NAMES_CSV"
if [[ ${#METRIC_NAMES[@]} -eq 0 ]]; then
  echo "--metric-names must not be empty" >&2
  exit 1
fi
for metric in "${METRIC_NAMES[@]}"; do
  if [[ -z "$metric" ]]; then
    echo "--metric-names contains empty entry" >&2
    exit 1
  fi
done

if [[ -z "$PRIMARY_METRIC" ]]; then
  PRIMARY_METRIC="${METRIC_NAMES[0]}"
fi
primary_ok=false
for metric in "${METRIC_NAMES[@]}"; do
  if [[ "$metric" == "$PRIMARY_METRIC" ]]; then
    primary_ok=true
    break
  fi
done
if [[ "$primary_ok" != true ]]; then
  echo "--primary-metric must be one of: $METRIC_NAMES_CSV" >&2
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
  awk -v arity="$SCENARIO_ARITY" \
      -v key="$key" '
    BEGIN { split(key, want, /\|/); }
    $1 ~ /^#/ { next }
    {
      if (NF < arity + 1) {
        next
      }
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
if [[ -n "$RATIO_LABEL" ]]; then
  ratio_expr="$RATIO_LABEL"
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
  IFS=':' read -r -a RAW_VALUES <<<"$item"
  if [[ ${#RAW_VALUES[@]} -ne "$SCENARIO_ARITY" ]]; then
    echo "Invalid scenario: $item (expected $SCENARIO_FORMAT)" >&2
    exit 1
  fi

  VALUES=("${RAW_VALUES[@]}")

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

  for metric in "${METRIC_NAMES[@]}"; do
    eval "iocoro_runs_${metric}=()"
    eval "asio_runs_${metric}=()"
  done

  for ((i = 0; i < ITERATIONS; ++i)); do
    line="$(bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$IOCORO_BIN" "${VALUES[@]}")"
    for metric in "${METRIC_NAMES[@]}"; do
      metric_value="$(bench_extract_metric "$metric" "$line")"
      if [[ -z "$metric_value" ]]; then
        echo "Failed to parse iocoro $metric: $line" >&2
        exit 1
      fi
      eval "iocoro_runs_${metric}+=(\"$metric_value\")"
    done
  done

  for ((i = 0; i < ITERATIONS; ++i)); do
    line="$(bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$ASIO_BIN" "${VALUES[@]}")"
    for metric in "${METRIC_NAMES[@]}"; do
      metric_value="$(bench_extract_metric "$metric" "$line")"
      if [[ -z "$metric_value" ]]; then
        echo "Failed to parse asio $metric: $line" >&2
        exit 1
      fi
      eval "asio_runs_${metric}+=(\"$metric_value\")"
    done
  done

  declare -A iocoro_medians=()
  declare -A asio_medians=()
  for metric in "${METRIC_NAMES[@]}"; do
    eval "iocoro_values=(\"\${iocoro_runs_${metric}[@]}\")"
    iocoro_medians["$metric"]="$(printf '%s\n' "${iocoro_values[@]}" | sort -n | bench_median_from_stdin)"

    eval "asio_values=(\"\${asio_runs_${metric}[@]}\")"
    asio_medians["$metric"]="$(printf '%s\n' "${asio_values[@]}" | sort -n | bench_median_from_stdin)"
  done

  primary_iocoro="${iocoro_medians[$PRIMARY_METRIC]}"
  primary_asio="${asio_medians[$PRIMARY_METRIC]}"

  if [[ "$RATIO_MODE" == "direct" ]]; then
    ratio="$(bench_compute_ratio "$primary_iocoro" "$primary_asio")"
  else
    ratio="$(bench_compute_ratio "$primary_asio" "$primary_iocoro")"
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
  table_rows+=("${row_prefix}|${primary_iocoro}|${primary_asio}|${ratio}|${min_ratio:-n/a}|${pass_label}")

  scenario_json_entry="{"
  for ((idx = 0; idx < SCENARIO_ARITY; ++idx)); do
    scenario_json_entry+="\"${SCENARIO_FIELDS[$idx]}\":${VALUES[$idx]},"
  done

  for metric in "${METRIC_NAMES[@]}"; do
    eval "iocoro_values=(\"\${iocoro_runs_${metric}[@]}\")"
    eval "asio_values=(\"\${asio_runs_${metric}[@]}\")"
    iocoro_runs_json="$(printf '%s\n' "${iocoro_values[@]}" | paste -sd, -)"
    asio_runs_json="$(printf '%s\n' "${asio_values[@]}" | paste -sd, -)"
    scenario_json_entry+="\"iocoro_${metric}_runs\":[${iocoro_runs_json}],"
    scenario_json_entry+="\"asio_${metric}_runs\":[${asio_runs_json}],"
    scenario_json_entry+="\"iocoro_${metric}_median\":${iocoro_medians[$metric]},"
    scenario_json_entry+="\"asio_${metric}_median\":${asio_medians[$metric]},"
  done

  if [[ -n "$min_ratio" ]]; then
    min_ratio_json="$min_ratio"
  else
    min_ratio_json="null"
  fi

  scenario_json_entry+="\"${RATIO_FIELD}\":${ratio},\"min_ratio\":${min_ratio_json},\"pass\":${pass_bool}}"
  scenario_json+=("$scenario_json_entry")

  if [[ ${#METRIC_NAMES[@]} -eq 1 ]]; then
    metric="${METRIC_NAMES[0]}"
    echo "  iocoro median $metric: ${iocoro_medians[$metric]}"
    echo "  asio   median $metric: ${asio_medians[$metric]}"
  else
    iocoro_line=""
    asio_line=""
    for metric in "${METRIC_NAMES[@]}"; do
      iocoro_line+="$metric=${iocoro_medians[$metric]} "
      asio_line+="$metric=${asio_medians[$metric]} "
    done
    echo "  iocoro medians: ${iocoro_line% }"
    echo "  asio   medians: ${asio_line% }"
  fi
  echo "  ratio ($ratio_expr): $ratio"
  if [[ -n "$min_ratio" ]]; then
    echo "  threshold: $min_ratio ($pass_label)"
  fi
  echo
done

echo "Summary"
header=("${SCENARIO_FIELDS[@]}" "iocoro_${PRIMARY_METRIC}_median" "asio_${PRIMARY_METRIC}_median" "$RATIO_FIELD" "min_ratio" "gate")
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
