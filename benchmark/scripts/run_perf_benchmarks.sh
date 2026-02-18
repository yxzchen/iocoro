#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
PROJECT_DIR="$(cd -- "${BENCH_DIR}/.." && pwd)"

bench_require_non_negative_int() {
  local name="$1"
  local value="$2"
  if ! [[ "$value" =~ ^[0-9]+$ ]]; then
    echo "$name must be a non-negative integer" >&2
    return 1
  fi
}

bench_require_positive_int() {
  local name="$1"
  local value="$2"
  bench_require_non_negative_int "$name" "$value" || return 1
  if [[ "$value" -le 0 ]]; then
    echo "$name must be > 0" >&2
    return 1
  fi
}

bench_to_abs_path() {
  local project_dir="$1"
  local path="$2"
  if [[ "$path" == /* ]]; then
    printf '%s\n' "$path"
  else
    printf '%s\n' "$project_dir/$path"
  fi
}

run_step_with_summary() {
  local label="$1"
  local summary_file="$2"
  shift 2

  set +e
  "$@" | tee "$summary_file"
  local code=$?
  set -e

  if [[ $code -ne 0 ]]; then
    FAILED_STEPS+=("$label")
  fi
}

run_step_no_summary() {
  local label="$1"
  shift

  set +e
  "$@"
  local code=$?
  set -e

  if [[ $code -ne 0 ]]; then
    FAILED_STEPS+=("$label")
  fi
}

suite_fields_csv() {
  local suite_id="$1"
  case "$suite_id" in
    tcp_roundtrip|tcp_latency|udp_send_receive)
      echo "sessions,msgs,msg_bytes"
      ;;
    tcp_connect_accept)
      echo "connections"
      ;;
    tcp_throughput)
      echo "sessions,bytes_per_session,chunk_bytes"
      ;;
    timer_churn)
      echo "sessions,waits"
      ;;
    thread_pool_scaling)
      echo "workers,tasks"
      ;;
    *)
      echo "Unknown suite id: $suite_id" >&2
      return 1
      ;;
  esac
}

field_in_list() {
  local field="$1"
  shift
  local item
  for item in "$@"; do
    if [[ "$item" == "$field" ]]; then
      return 0
    fi
  done
  return 1
}

trim_spaces() {
  local s="$1"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  printf '%s\n' "$s"
}

serialize_scenario_rows() {
  local suite_id="$1"
  local rows_name="$2"
  local out_name="$3"

  local -n rows="$rows_name"
  local -n out="$out_name"

  local fields_csv
  fields_csv="$(suite_fields_csv "$suite_id")" || return 1

  local -a fields=()
  IFS=',' read -r -a fields <<<"$fields_csv"

  if [[ ${#rows[@]} -eq 0 ]]; then
    echo "${suite_id}: SCENARIO_ROWS must not be empty" >&2
    return 1
  fi

  local serialized=""
  local idx=0
  local row_raw row text_without_label token key value tuple
  for row_raw in "${rows[@]}"; do
    idx=$((idx + 1))
    row="$row_raw"

    if [[ "$row" == *:* ]]; then
      local prefix="${row%%:*}"
      if [[ "$prefix" != *=* ]]; then
        row="${row#*:}"
      fi
    fi

    row="$(trim_spaces "$row")"
    if [[ -z "$row" ]]; then
      echo "${suite_id}: SCENARIO_ROWS[$idx] is empty" >&2
      return 1
    fi

    declare -A kv=()
    for token in $row; do
      if [[ "$token" != *=* ]]; then
        echo "${suite_id}: SCENARIO_ROWS[$idx] invalid token: $token (expected key=value)" >&2
        return 1
      fi
      key="${token%%=*}"
      value="${token#*=}"
      if ! field_in_list "$key" "${fields[@]}"; then
        echo "${suite_id}: SCENARIO_ROWS[$idx] unknown field: $key" >&2
        echo "Expected fields: $fields_csv" >&2
        return 1
      fi
      bench_require_positive_int "${suite_id}.SCENARIO_ROWS[$idx].$key" "$value"
      kv["$key"]="$value"
    done

    local -a ordered_values=()
    local f
    for f in "${fields[@]}"; do
      if [[ -z "${kv[$f]+x}" ]]; then
        echo "${suite_id}: SCENARIO_ROWS[$idx] missing field: $f" >&2
        echo "Expected fields: $fields_csv" >&2
        return 1
      fi
      ordered_values+=("${kv[$f]}")
    done

    tuple="$(IFS=':'; echo "${ordered_values[*]}")"
    serialized+="${serialized:+,}${tuple}"
  done

  out="$serialized"
}

load_suite_config() {
  local suite_id="$1"
  local file="$2"
  local -n out_iterations="$3"
  local -n out_warmup="$4"
  local -n out_timeout_sec="$5"
  local -n out_scenarios="$6"

  if [[ ! -f "$file" ]]; then
    echo "Missing suite config for ${suite_id}: ${file}" >&2
    return 1
  fi

  local ITERATIONS=""
  local WARMUP=""
  local TIMEOUT_SEC=""
  local SCENARIOS=""
  local -a SCENARIO_ROWS=()

  # shellcheck disable=SC1090
  source "$file"

  if [[ -z "$ITERATIONS" || -z "$WARMUP" || -z "$TIMEOUT_SEC" ]]; then
    echo "Invalid suite config for ${suite_id}: ${file}" >&2
    echo "Required keys: ITERATIONS, WARMUP, TIMEOUT_SEC, and SCENARIO_ROWS (or SCENARIOS for legacy format)" >&2
    return 1
  fi

  bench_require_positive_int "${suite_id}.ITERATIONS" "$ITERATIONS"
  bench_require_non_negative_int "${suite_id}.WARMUP" "$WARMUP"
  bench_require_non_negative_int "${suite_id}.TIMEOUT_SEC" "$TIMEOUT_SEC"

  if [[ ${#SCENARIO_ROWS[@]} -gt 0 ]]; then
    serialize_scenario_rows "$suite_id" SCENARIO_ROWS scenarios_csv || return 1
  elif [[ -n "$SCENARIOS" ]]; then
    scenarios_csv="$SCENARIOS"
  else
    echo "Invalid suite config for ${suite_id}: ${file}" >&2
    echo "Missing scenario definition: set SCENARIO_ROWS (preferred) or SCENARIOS" >&2
    return 1
  fi

  out_iterations="$ITERATIONS"
  out_warmup="$WARMUP"
  out_timeout_sec="$TIMEOUT_SEC"
  out_scenarios="$scenarios_csv"
}

BUILD_DIR="$PROJECT_DIR/build"
CONF_DIR="$PROJECT_DIR/benchmark/conf"

ITERATIONS_OVERRIDE=""
WARMUP_OVERRIDE=""
TIMEOUT_SEC_OVERRIDE=""

TCP_ROUNDTRIP_SCENARIOS_OVERRIDE=""
TCP_LATENCY_SCENARIOS_OVERRIDE=""
TCP_CONNECT_ACCEPT_SCENARIOS_OVERRIDE=""
TCP_THROUGHPUT_SCENARIOS_OVERRIDE=""
UDP_SEND_RECEIVE_SCENARIOS_OVERRIDE=""
TIMER_CHURN_SCENARIOS_OVERRIDE=""
THREAD_POOL_SCALING_SCENARIOS_OVERRIDE=""

TCP_ROUNDTRIP_TIMEOUT_OVERRIDE=""
TCP_LATENCY_TIMEOUT_OVERRIDE=""
TCP_CONNECT_ACCEPT_TIMEOUT_OVERRIDE=""
TCP_THROUGHPUT_TIMEOUT_OVERRIDE=""
UDP_SEND_RECEIVE_TIMEOUT_OVERRIDE=""
TIMER_CHURN_TIMEOUT_OVERRIDE=""
THREAD_POOL_SCALING_TIMEOUT_OVERRIDE=""

TCP_ROUNDTRIP_CONFIG=""
TCP_LATENCY_CONFIG=""
TCP_CONNECT_ACCEPT_CONFIG=""
TCP_THROUGHPUT_CONFIG=""
UDP_SEND_RECEIVE_CONFIG=""
TIMER_CHURN_CONFIG=""
THREAD_POOL_SCALING_CONFIG=""

ENABLE_SCHEMA_VALIDATE=true

TCP_ROUNDTRIP_REPORT="$PROJECT_DIR/benchmark/reports/tcp_roundtrip.report.json"
TCP_LATENCY_REPORT="$PROJECT_DIR/benchmark/reports/tcp_latency.report.json"
TCP_CONNECT_ACCEPT_REPORT="$PROJECT_DIR/benchmark/reports/tcp_connect_accept.report.json"
TCP_THROUGHPUT_REPORT="$PROJECT_DIR/benchmark/reports/tcp_throughput.report.json"
UDP_SEND_RECEIVE_REPORT="$PROJECT_DIR/benchmark/reports/udp_send_receive.report.json"
TIMER_CHURN_REPORT="$PROJECT_DIR/benchmark/reports/timer_churn.report.json"
THREAD_POOL_SCALING_REPORT="$PROJECT_DIR/benchmark/reports/thread_pool_scaling.report.json"

FAILED_STEPS=()

usage() {
  cat <<'EOF2'
Usage: benchmark/scripts/run_perf_benchmarks.sh [options]

Run all performance benchmark suites:
- tcp_roundtrip
- tcp_latency
- tcp_connect_accept
- tcp_throughput
- udp_send_receive
- timer_churn
- thread_pool_scaling

Suite defaults come from one file per suite under `benchmark/conf/*.conf`.
Each file must define: ITERATIONS, WARMUP, TIMEOUT_SEC, SCENARIO_ROWS.

SCENARIO_ROWS format (friendly):
  SCENARIO_ROWS=(
    "key1=value1 key2=value2 ..."
    "key1=value1 key2=value2 ..."
  )

Options:
  --build-dir DIR                         CMake build dir (default: ./build)
  --conf-dir DIR                          Directory containing suite config files (default: benchmark/conf)
  --suite-config-dir DIR                  Alias of --conf-dir (deprecated)

  --iterations N                          Override ITERATIONS for all suites
  --warmup N                              Override WARMUP for all suites
  --timeout-sec N                         Override TIMEOUT_SEC for all suites

  --tcp-roundtrip-config FILE             Config file for tcp_roundtrip
  --tcp-latency-config FILE               Config file for tcp_latency
  --tcp-connect-accept-config FILE        Config file for tcp_connect_accept
  --tcp-throughput-config FILE            Config file for tcp_throughput
  --udp-send-receive-config FILE          Config file for udp_send_receive
  --timer-churn-config FILE               Config file for timer_churn
  --thread-pool-scaling-config FILE       Config file for thread_pool_scaling

  --tcp-roundtrip-scenarios LIST          Override SCENARIOS for tcp_roundtrip
  --tcp-latency-scenarios LIST            Override SCENARIOS for tcp_latency
  --tcp-connect-accept-scenarios LIST     Override SCENARIOS for tcp_connect_accept
  --tcp-throughput-scenarios LIST         Override SCENARIOS for tcp_throughput
  --udp-send-receive-scenarios LIST       Override SCENARIOS for udp_send_receive
  --timer-churn-scenarios LIST            Override SCENARIOS for timer_churn
  --thread-pool-scaling-scenarios LIST    Override SCENARIOS for thread_pool_scaling

  --tcp-roundtrip-timeout-sec N           Override TIMEOUT_SEC for tcp_roundtrip
  --tcp-latency-timeout-sec N             Override TIMEOUT_SEC for tcp_latency
  --tcp-connect-accept-timeout-sec N      Override TIMEOUT_SEC for tcp_connect_accept
  --tcp-throughput-timeout-sec N          Override TIMEOUT_SEC for tcp_throughput
  --udp-send-receive-timeout-sec N        Override TIMEOUT_SEC for udp_send_receive
  --timer-churn-timeout-sec N             Override TIMEOUT_SEC for timer_churn
  --thread-pool-scaling-timeout-sec N     Override TIMEOUT_SEC for thread_pool_scaling

  --tcp-roundtrip-report FILE             Report path (default: benchmark/reports/tcp_roundtrip.report.json)
  --tcp-latency-report FILE               Report path (default: benchmark/reports/tcp_latency.report.json)
  --tcp-connect-accept-report FILE        Report path (default: benchmark/reports/tcp_connect_accept.report.json)
  --tcp-throughput-report FILE            Report path (default: benchmark/reports/tcp_throughput.report.json)
  --udp-send-receive-report FILE          Report path (default: benchmark/reports/udp_send_receive.report.json)
  --timer-churn-report FILE               Report path (default: benchmark/reports/timer_churn.report.json)
  --thread-pool-scaling-report FILE       Report path (default: benchmark/reports/thread_pool_scaling.report.json)

  --no-schema-validate                    Skip JSON schema validation
  -h, --help                              Show this help
EOF2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --conf-dir|--suite-config-dir)
      CONF_DIR="$2"
      shift 2
      ;;

    --iterations)
      ITERATIONS_OVERRIDE="$2"
      shift 2
      ;;
    --warmup)
      WARMUP_OVERRIDE="$2"
      shift 2
      ;;
    --timeout-sec)
      TIMEOUT_SEC_OVERRIDE="$2"
      shift 2
      ;;

    --tcp-roundtrip-config)
      TCP_ROUNDTRIP_CONFIG="$2"
      shift 2
      ;;
    --tcp-latency-config)
      TCP_LATENCY_CONFIG="$2"
      shift 2
      ;;
    --tcp-connect-accept-config)
      TCP_CONNECT_ACCEPT_CONFIG="$2"
      shift 2
      ;;
    --tcp-throughput-config)
      TCP_THROUGHPUT_CONFIG="$2"
      shift 2
      ;;
    --udp-send-receive-config)
      UDP_SEND_RECEIVE_CONFIG="$2"
      shift 2
      ;;
    --timer-churn-config)
      TIMER_CHURN_CONFIG="$2"
      shift 2
      ;;
    --thread-pool-scaling-config)
      THREAD_POOL_SCALING_CONFIG="$2"
      shift 2
      ;;

    --tcp-roundtrip-scenarios)
      TCP_ROUNDTRIP_SCENARIOS_OVERRIDE="$2"
      shift 2
      ;;
    --tcp-latency-scenarios)
      TCP_LATENCY_SCENARIOS_OVERRIDE="$2"
      shift 2
      ;;
    --tcp-connect-accept-scenarios)
      TCP_CONNECT_ACCEPT_SCENARIOS_OVERRIDE="$2"
      shift 2
      ;;
    --tcp-throughput-scenarios)
      TCP_THROUGHPUT_SCENARIOS_OVERRIDE="$2"
      shift 2
      ;;
    --udp-send-receive-scenarios)
      UDP_SEND_RECEIVE_SCENARIOS_OVERRIDE="$2"
      shift 2
      ;;
    --timer-churn-scenarios)
      TIMER_CHURN_SCENARIOS_OVERRIDE="$2"
      shift 2
      ;;
    --thread-pool-scaling-scenarios)
      THREAD_POOL_SCALING_SCENARIOS_OVERRIDE="$2"
      shift 2
      ;;

    --tcp-roundtrip-timeout-sec)
      TCP_ROUNDTRIP_TIMEOUT_OVERRIDE="$2"
      shift 2
      ;;
    --tcp-latency-timeout-sec)
      TCP_LATENCY_TIMEOUT_OVERRIDE="$2"
      shift 2
      ;;
    --tcp-connect-accept-timeout-sec)
      TCP_CONNECT_ACCEPT_TIMEOUT_OVERRIDE="$2"
      shift 2
      ;;
    --tcp-throughput-timeout-sec)
      TCP_THROUGHPUT_TIMEOUT_OVERRIDE="$2"
      shift 2
      ;;
    --udp-send-receive-timeout-sec)
      UDP_SEND_RECEIVE_TIMEOUT_OVERRIDE="$2"
      shift 2
      ;;
    --timer-churn-timeout-sec)
      TIMER_CHURN_TIMEOUT_OVERRIDE="$2"
      shift 2
      ;;
    --thread-pool-scaling-timeout-sec)
      THREAD_POOL_SCALING_TIMEOUT_OVERRIDE="$2"
      shift 2
      ;;

    --tcp-roundtrip-report)
      TCP_ROUNDTRIP_REPORT="$2"
      shift 2
      ;;
    --tcp-latency-report)
      TCP_LATENCY_REPORT="$2"
      shift 2
      ;;
    --tcp-connect-accept-report)
      TCP_CONNECT_ACCEPT_REPORT="$2"
      shift 2
      ;;
    --tcp-throughput-report)
      TCP_THROUGHPUT_REPORT="$2"
      shift 2
      ;;
    --udp-send-receive-report)
      UDP_SEND_RECEIVE_REPORT="$2"
      shift 2
      ;;
    --timer-churn-report)
      TIMER_CHURN_REPORT="$2"
      shift 2
      ;;
    --thread-pool-scaling-report)
      THREAD_POOL_SCALING_REPORT="$2"
      shift 2
      ;;

    --no-schema-validate)
      ENABLE_SCHEMA_VALIDATE=false
      shift
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

if [[ -n "$ITERATIONS_OVERRIDE" ]]; then
  bench_require_positive_int "--iterations" "$ITERATIONS_OVERRIDE"
fi
if [[ -n "$WARMUP_OVERRIDE" ]]; then
  bench_require_non_negative_int "--warmup" "$WARMUP_OVERRIDE"
fi
if [[ -n "$TIMEOUT_SEC_OVERRIDE" ]]; then
  bench_require_non_negative_int "--timeout-sec" "$TIMEOUT_SEC_OVERRIDE"
fi

for timeout_pair in \
  "--tcp-roundtrip-timeout-sec:$TCP_ROUNDTRIP_TIMEOUT_OVERRIDE" \
  "--tcp-latency-timeout-sec:$TCP_LATENCY_TIMEOUT_OVERRIDE" \
  "--tcp-connect-accept-timeout-sec:$TCP_CONNECT_ACCEPT_TIMEOUT_OVERRIDE" \
  "--tcp-throughput-timeout-sec:$TCP_THROUGHPUT_TIMEOUT_OVERRIDE" \
  "--udp-send-receive-timeout-sec:$UDP_SEND_RECEIVE_TIMEOUT_OVERRIDE" \
  "--timer-churn-timeout-sec:$TIMER_CHURN_TIMEOUT_OVERRIDE" \
  "--thread-pool-scaling-timeout-sec:$THREAD_POOL_SCALING_TIMEOUT_OVERRIDE"; do
  IFS=':' read -r timeout_name timeout_value <<<"$timeout_pair"
  if [[ -n "$timeout_value" ]]; then
    bench_require_non_negative_int "$timeout_name" "$timeout_value"
  fi
done

BUILD_DIR="$(bench_to_abs_path "$PROJECT_DIR" "$BUILD_DIR")"
CONF_DIR="$(bench_to_abs_path "$PROJECT_DIR" "$CONF_DIR")"

: "${TCP_ROUNDTRIP_CONFIG:=$CONF_DIR/tcp_roundtrip.conf}"
: "${TCP_LATENCY_CONFIG:=$CONF_DIR/tcp_latency.conf}"
: "${TCP_CONNECT_ACCEPT_CONFIG:=$CONF_DIR/tcp_connect_accept.conf}"
: "${TCP_THROUGHPUT_CONFIG:=$CONF_DIR/tcp_throughput.conf}"
: "${UDP_SEND_RECEIVE_CONFIG:=$CONF_DIR/udp_send_receive.conf}"
: "${TIMER_CHURN_CONFIG:=$CONF_DIR/timer_churn.conf}"
: "${THREAD_POOL_SCALING_CONFIG:=$CONF_DIR/thread_pool_scaling.conf}"

TCP_ROUNDTRIP_CONFIG="$(bench_to_abs_path "$PROJECT_DIR" "$TCP_ROUNDTRIP_CONFIG")"
TCP_LATENCY_CONFIG="$(bench_to_abs_path "$PROJECT_DIR" "$TCP_LATENCY_CONFIG")"
TCP_CONNECT_ACCEPT_CONFIG="$(bench_to_abs_path "$PROJECT_DIR" "$TCP_CONNECT_ACCEPT_CONFIG")"
TCP_THROUGHPUT_CONFIG="$(bench_to_abs_path "$PROJECT_DIR" "$TCP_THROUGHPUT_CONFIG")"
UDP_SEND_RECEIVE_CONFIG="$(bench_to_abs_path "$PROJECT_DIR" "$UDP_SEND_RECEIVE_CONFIG")"
TIMER_CHURN_CONFIG="$(bench_to_abs_path "$PROJECT_DIR" "$TIMER_CHURN_CONFIG")"
THREAD_POOL_SCALING_CONFIG="$(bench_to_abs_path "$PROJECT_DIR" "$THREAD_POOL_SCALING_CONFIG")"

TCP_ROUNDTRIP_REPORT="$(bench_to_abs_path "$PROJECT_DIR" "$TCP_ROUNDTRIP_REPORT")"
TCP_LATENCY_REPORT="$(bench_to_abs_path "$PROJECT_DIR" "$TCP_LATENCY_REPORT")"
TCP_CONNECT_ACCEPT_REPORT="$(bench_to_abs_path "$PROJECT_DIR" "$TCP_CONNECT_ACCEPT_REPORT")"
TCP_THROUGHPUT_REPORT="$(bench_to_abs_path "$PROJECT_DIR" "$TCP_THROUGHPUT_REPORT")"
UDP_SEND_RECEIVE_REPORT="$(bench_to_abs_path "$PROJECT_DIR" "$UDP_SEND_RECEIVE_REPORT")"
TIMER_CHURN_REPORT="$(bench_to_abs_path "$PROJECT_DIR" "$TIMER_CHURN_REPORT")"
THREAD_POOL_SCALING_REPORT="$(bench_to_abs_path "$PROJECT_DIR" "$THREAD_POOL_SCALING_REPORT")"

load_suite_config "tcp_roundtrip" "$TCP_ROUNDTRIP_CONFIG" cfg_tcp_roundtrip_iterations cfg_tcp_roundtrip_warmup cfg_tcp_roundtrip_timeout cfg_tcp_roundtrip_scenarios
load_suite_config "tcp_latency" "$TCP_LATENCY_CONFIG" cfg_tcp_latency_iterations cfg_tcp_latency_warmup cfg_tcp_latency_timeout cfg_tcp_latency_scenarios
load_suite_config "tcp_connect_accept" "$TCP_CONNECT_ACCEPT_CONFIG" cfg_tcp_connect_accept_iterations cfg_tcp_connect_accept_warmup cfg_tcp_connect_accept_timeout cfg_tcp_connect_accept_scenarios
load_suite_config "tcp_throughput" "$TCP_THROUGHPUT_CONFIG" cfg_tcp_throughput_iterations cfg_tcp_throughput_warmup cfg_tcp_throughput_timeout cfg_tcp_throughput_scenarios
load_suite_config "udp_send_receive" "$UDP_SEND_RECEIVE_CONFIG" cfg_udp_send_receive_iterations cfg_udp_send_receive_warmup cfg_udp_send_receive_timeout cfg_udp_send_receive_scenarios
load_suite_config "timer_churn" "$TIMER_CHURN_CONFIG" cfg_timer_churn_iterations cfg_timer_churn_warmup cfg_timer_churn_timeout cfg_timer_churn_scenarios
load_suite_config "thread_pool_scaling" "$THREAD_POOL_SCALING_CONFIG" cfg_thread_pool_scaling_iterations cfg_thread_pool_scaling_warmup cfg_thread_pool_scaling_timeout cfg_thread_pool_scaling_scenarios

tcp_roundtrip_iterations="$cfg_tcp_roundtrip_iterations"
tcp_latency_iterations="$cfg_tcp_latency_iterations"
tcp_connect_accept_iterations="$cfg_tcp_connect_accept_iterations"
tcp_throughput_iterations="$cfg_tcp_throughput_iterations"
udp_send_receive_iterations="$cfg_udp_send_receive_iterations"
timer_churn_iterations="$cfg_timer_churn_iterations"
thread_pool_scaling_iterations="$cfg_thread_pool_scaling_iterations"

tcp_roundtrip_warmup="$cfg_tcp_roundtrip_warmup"
tcp_latency_warmup="$cfg_tcp_latency_warmup"
tcp_connect_accept_warmup="$cfg_tcp_connect_accept_warmup"
tcp_throughput_warmup="$cfg_tcp_throughput_warmup"
udp_send_receive_warmup="$cfg_udp_send_receive_warmup"
timer_churn_warmup="$cfg_timer_churn_warmup"
thread_pool_scaling_warmup="$cfg_thread_pool_scaling_warmup"

tcp_roundtrip_timeout="$cfg_tcp_roundtrip_timeout"
tcp_latency_timeout="$cfg_tcp_latency_timeout"
tcp_connect_accept_timeout="$cfg_tcp_connect_accept_timeout"
tcp_throughput_timeout="$cfg_tcp_throughput_timeout"
udp_send_receive_timeout="$cfg_udp_send_receive_timeout"
timer_churn_timeout="$cfg_timer_churn_timeout"
thread_pool_scaling_timeout="$cfg_thread_pool_scaling_timeout"

tcp_roundtrip_scenarios="$cfg_tcp_roundtrip_scenarios"
tcp_latency_scenarios="$cfg_tcp_latency_scenarios"
tcp_connect_accept_scenarios="$cfg_tcp_connect_accept_scenarios"
tcp_throughput_scenarios="$cfg_tcp_throughput_scenarios"
udp_send_receive_scenarios="$cfg_udp_send_receive_scenarios"
timer_churn_scenarios="$cfg_timer_churn_scenarios"
thread_pool_scaling_scenarios="$cfg_thread_pool_scaling_scenarios"

if [[ -n "$ITERATIONS_OVERRIDE" ]]; then
  tcp_roundtrip_iterations="$ITERATIONS_OVERRIDE"
  tcp_latency_iterations="$ITERATIONS_OVERRIDE"
  tcp_connect_accept_iterations="$ITERATIONS_OVERRIDE"
  tcp_throughput_iterations="$ITERATIONS_OVERRIDE"
  udp_send_receive_iterations="$ITERATIONS_OVERRIDE"
  timer_churn_iterations="$ITERATIONS_OVERRIDE"
  thread_pool_scaling_iterations="$ITERATIONS_OVERRIDE"
fi

if [[ -n "$WARMUP_OVERRIDE" ]]; then
  tcp_roundtrip_warmup="$WARMUP_OVERRIDE"
  tcp_latency_warmup="$WARMUP_OVERRIDE"
  tcp_connect_accept_warmup="$WARMUP_OVERRIDE"
  tcp_throughput_warmup="$WARMUP_OVERRIDE"
  udp_send_receive_warmup="$WARMUP_OVERRIDE"
  timer_churn_warmup="$WARMUP_OVERRIDE"
  thread_pool_scaling_warmup="$WARMUP_OVERRIDE"
fi

if [[ -n "$TIMEOUT_SEC_OVERRIDE" ]]; then
  tcp_roundtrip_timeout="$TIMEOUT_SEC_OVERRIDE"
  tcp_latency_timeout="$TIMEOUT_SEC_OVERRIDE"
  tcp_connect_accept_timeout="$TIMEOUT_SEC_OVERRIDE"
  tcp_throughput_timeout="$TIMEOUT_SEC_OVERRIDE"
  udp_send_receive_timeout="$TIMEOUT_SEC_OVERRIDE"
  timer_churn_timeout="$TIMEOUT_SEC_OVERRIDE"
  thread_pool_scaling_timeout="$TIMEOUT_SEC_OVERRIDE"
fi

if [[ -n "$TCP_ROUNDTRIP_TIMEOUT_OVERRIDE" ]]; then tcp_roundtrip_timeout="$TCP_ROUNDTRIP_TIMEOUT_OVERRIDE"; fi
if [[ -n "$TCP_LATENCY_TIMEOUT_OVERRIDE" ]]; then tcp_latency_timeout="$TCP_LATENCY_TIMEOUT_OVERRIDE"; fi
if [[ -n "$TCP_CONNECT_ACCEPT_TIMEOUT_OVERRIDE" ]]; then tcp_connect_accept_timeout="$TCP_CONNECT_ACCEPT_TIMEOUT_OVERRIDE"; fi
if [[ -n "$TCP_THROUGHPUT_TIMEOUT_OVERRIDE" ]]; then tcp_throughput_timeout="$TCP_THROUGHPUT_TIMEOUT_OVERRIDE"; fi
if [[ -n "$UDP_SEND_RECEIVE_TIMEOUT_OVERRIDE" ]]; then udp_send_receive_timeout="$UDP_SEND_RECEIVE_TIMEOUT_OVERRIDE"; fi
if [[ -n "$TIMER_CHURN_TIMEOUT_OVERRIDE" ]]; then timer_churn_timeout="$TIMER_CHURN_TIMEOUT_OVERRIDE"; fi
if [[ -n "$THREAD_POOL_SCALING_TIMEOUT_OVERRIDE" ]]; then thread_pool_scaling_timeout="$THREAD_POOL_SCALING_TIMEOUT_OVERRIDE"; fi

if [[ -n "$TCP_ROUNDTRIP_SCENARIOS_OVERRIDE" ]]; then tcp_roundtrip_scenarios="$TCP_ROUNDTRIP_SCENARIOS_OVERRIDE"; fi
if [[ -n "$TCP_LATENCY_SCENARIOS_OVERRIDE" ]]; then tcp_latency_scenarios="$TCP_LATENCY_SCENARIOS_OVERRIDE"; fi
if [[ -n "$TCP_CONNECT_ACCEPT_SCENARIOS_OVERRIDE" ]]; then tcp_connect_accept_scenarios="$TCP_CONNECT_ACCEPT_SCENARIOS_OVERRIDE"; fi
if [[ -n "$TCP_THROUGHPUT_SCENARIOS_OVERRIDE" ]]; then tcp_throughput_scenarios="$TCP_THROUGHPUT_SCENARIOS_OVERRIDE"; fi
if [[ -n "$UDP_SEND_RECEIVE_SCENARIOS_OVERRIDE" ]]; then udp_send_receive_scenarios="$UDP_SEND_RECEIVE_SCENARIOS_OVERRIDE"; fi
if [[ -n "$TIMER_CHURN_SCENARIOS_OVERRIDE" ]]; then timer_churn_scenarios="$TIMER_CHURN_SCENARIOS_OVERRIDE"; fi
if [[ -n "$THREAD_POOL_SCALING_SCENARIOS_OVERRIDE" ]]; then thread_pool_scaling_scenarios="$THREAD_POOL_SCALING_SCENARIOS_OVERRIDE"; fi

TCP_ROUNDTRIP_SUMMARY="$(dirname -- "$TCP_ROUNDTRIP_REPORT")/tcp_roundtrip.summary.txt"
TCP_LATENCY_SUMMARY="$(dirname -- "$TCP_LATENCY_REPORT")/tcp_latency.summary.txt"
TCP_CONNECT_ACCEPT_SUMMARY="$(dirname -- "$TCP_CONNECT_ACCEPT_REPORT")/tcp_connect_accept.summary.txt"
TCP_THROUGHPUT_SUMMARY="$(dirname -- "$TCP_THROUGHPUT_REPORT")/tcp_throughput.summary.txt"
UDP_SEND_RECEIVE_SUMMARY="$(dirname -- "$UDP_SEND_RECEIVE_REPORT")/udp_send_receive.summary.txt"
TIMER_CHURN_SUMMARY="$(dirname -- "$TIMER_CHURN_REPORT")/timer_churn.summary.txt"
THREAD_POOL_SCALING_SUMMARY="$(dirname -- "$THREAD_POOL_SCALING_REPORT")/thread_pool_scaling.summary.txt"

mkdir -p "$(dirname -- "$TCP_ROUNDTRIP_REPORT")"
mkdir -p "$(dirname -- "$TCP_LATENCY_REPORT")"
mkdir -p "$(dirname -- "$TCP_CONNECT_ACCEPT_REPORT")"
mkdir -p "$(dirname -- "$TCP_THROUGHPUT_REPORT")"
mkdir -p "$(dirname -- "$UDP_SEND_RECEIVE_REPORT")"
mkdir -p "$(dirname -- "$TIMER_CHURN_REPORT")"
mkdir -p "$(dirname -- "$THREAD_POOL_SCALING_REPORT")"

suite_tcp_roundtrip_cmd=(
  "$SCRIPT_DIR/suites/run_perf_tcp_roundtrip.sh"
  --build-dir "$BUILD_DIR"
  --iterations "$tcp_roundtrip_iterations"
  --warmup "$tcp_roundtrip_warmup"
  --run-timeout-sec "$tcp_roundtrip_timeout"
  --scenarios "$tcp_roundtrip_scenarios"
  --report "$TCP_ROUNDTRIP_REPORT"
)

suite_tcp_latency_cmd=(
  "$SCRIPT_DIR/suites/run_perf_tcp_latency.sh"
  --build-dir "$BUILD_DIR"
  --iterations "$tcp_latency_iterations"
  --warmup "$tcp_latency_warmup"
  --run-timeout-sec "$tcp_latency_timeout"
  --scenarios "$tcp_latency_scenarios"
  --report "$TCP_LATENCY_REPORT"
)

suite_tcp_connect_accept_cmd=(
  "$SCRIPT_DIR/suites/run_perf_tcp_connect_accept.sh"
  --build-dir "$BUILD_DIR"
  --iterations "$tcp_connect_accept_iterations"
  --warmup "$tcp_connect_accept_warmup"
  --run-timeout-sec "$tcp_connect_accept_timeout"
  --scenarios "$tcp_connect_accept_scenarios"
  --report "$TCP_CONNECT_ACCEPT_REPORT"
)

suite_tcp_throughput_cmd=(
  "$SCRIPT_DIR/suites/run_perf_tcp_throughput.sh"
  --build-dir "$BUILD_DIR"
  --iterations "$tcp_throughput_iterations"
  --warmup "$tcp_throughput_warmup"
  --run-timeout-sec "$tcp_throughput_timeout"
  --scenarios "$tcp_throughput_scenarios"
  --report "$TCP_THROUGHPUT_REPORT"
)

suite_udp_send_receive_cmd=(
  "$SCRIPT_DIR/suites/run_perf_udp_send_receive.sh"
  --build-dir "$BUILD_DIR"
  --iterations "$udp_send_receive_iterations"
  --warmup "$udp_send_receive_warmup"
  --run-timeout-sec "$udp_send_receive_timeout"
  --scenarios "$udp_send_receive_scenarios"
  --report "$UDP_SEND_RECEIVE_REPORT"
)

suite_timer_churn_cmd=(
  "$SCRIPT_DIR/suites/run_perf_timer_churn.sh"
  --build-dir "$BUILD_DIR"
  --iterations "$timer_churn_iterations"
  --warmup "$timer_churn_warmup"
  --run-timeout-sec "$timer_churn_timeout"
  --scenarios "$timer_churn_scenarios"
  --report "$TIMER_CHURN_REPORT"
)

suite_thread_pool_scaling_cmd=(
  "$SCRIPT_DIR/suites/run_perf_thread_pool_scaling.sh"
  --build-dir "$BUILD_DIR"
  --iterations "$thread_pool_scaling_iterations"
  --warmup "$thread_pool_scaling_warmup"
  --run-timeout-sec "$thread_pool_scaling_timeout"
  --scenarios "$thread_pool_scaling_scenarios"
  --report "$THREAD_POOL_SCALING_REPORT"
)

echo "Running performance benchmark suites"
echo "  build_dir: $BUILD_DIR"
echo "  conf_dir: $CONF_DIR"
echo "  schema_validate: $ENABLE_SCHEMA_VALIDATE"
echo

run_step_with_summary "suite_tcp_roundtrip" "$TCP_ROUNDTRIP_SUMMARY" "${suite_tcp_roundtrip_cmd[@]}"
run_step_with_summary "suite_tcp_latency" "$TCP_LATENCY_SUMMARY" "${suite_tcp_latency_cmd[@]}"
run_step_with_summary "suite_tcp_connect_accept" "$TCP_CONNECT_ACCEPT_SUMMARY" "${suite_tcp_connect_accept_cmd[@]}"
run_step_with_summary "suite_tcp_throughput" "$TCP_THROUGHPUT_SUMMARY" "${suite_tcp_throughput_cmd[@]}"
run_step_with_summary "suite_udp_send_receive" "$UDP_SEND_RECEIVE_SUMMARY" "${suite_udp_send_receive_cmd[@]}"
run_step_with_summary "suite_timer_churn" "$TIMER_CHURN_SUMMARY" "${suite_timer_churn_cmd[@]}"
run_step_with_summary "suite_thread_pool_scaling" "$THREAD_POOL_SCALING_SUMMARY" "${suite_thread_pool_scaling_cmd[@]}"

if [[ "$ENABLE_SCHEMA_VALIDATE" == true ]]; then
  run_step_no_summary "schema_tcp_roundtrip" python3 "$SCRIPT_DIR/validate_benchmark_report.py" \
    --schema "$PROJECT_DIR/benchmark/schemas/tcp_roundtrip.schema.json" \
    --report "$TCP_ROUNDTRIP_REPORT"

  run_step_no_summary "schema_tcp_latency" python3 "$SCRIPT_DIR/validate_benchmark_report.py" \
    --schema "$PROJECT_DIR/benchmark/schemas/tcp_latency.schema.json" \
    --report "$TCP_LATENCY_REPORT"

  run_step_no_summary "schema_tcp_connect_accept" python3 "$SCRIPT_DIR/validate_benchmark_report.py" \
    --schema "$PROJECT_DIR/benchmark/schemas/tcp_connect_accept.schema.json" \
    --report "$TCP_CONNECT_ACCEPT_REPORT"

  run_step_no_summary "schema_tcp_throughput" python3 "$SCRIPT_DIR/validate_benchmark_report.py" \
    --schema "$PROJECT_DIR/benchmark/schemas/tcp_throughput.schema.json" \
    --report "$TCP_THROUGHPUT_REPORT"

  run_step_no_summary "schema_udp_send_receive" python3 "$SCRIPT_DIR/validate_benchmark_report.py" \
    --schema "$PROJECT_DIR/benchmark/schemas/udp_send_receive.schema.json" \
    --report "$UDP_SEND_RECEIVE_REPORT"

  run_step_no_summary "schema_timer_churn" python3 "$SCRIPT_DIR/validate_benchmark_report.py" \
    --schema "$PROJECT_DIR/benchmark/schemas/timer_churn.schema.json" \
    --report "$TIMER_CHURN_REPORT"

  run_step_no_summary "schema_thread_pool_scaling" python3 "$SCRIPT_DIR/validate_benchmark_report.py" \
    --schema "$PROJECT_DIR/benchmark/schemas/thread_pool_scaling.schema.json" \
    --report "$THREAD_POOL_SCALING_REPORT"
fi

echo
echo "Performance benchmark suites completed"
echo "  tcp_roundtrip report: $TCP_ROUNDTRIP_REPORT"
echo "  tcp_latency report: $TCP_LATENCY_REPORT"
echo "  tcp_connect_accept report: $TCP_CONNECT_ACCEPT_REPORT"
echo "  tcp_throughput report: $TCP_THROUGHPUT_REPORT"
echo "  udp_send_receive report: $UDP_SEND_RECEIVE_REPORT"
echo "  timer_churn report: $TIMER_CHURN_REPORT"
echo "  thread_pool_scaling report: $THREAD_POOL_SCALING_REPORT"
echo "  tcp_roundtrip summary: $TCP_ROUNDTRIP_SUMMARY"
echo "  tcp_latency summary: $TCP_LATENCY_SUMMARY"
echo "  tcp_connect_accept summary: $TCP_CONNECT_ACCEPT_SUMMARY"
echo "  tcp_throughput summary: $TCP_THROUGHPUT_SUMMARY"
echo "  udp_send_receive summary: $UDP_SEND_RECEIVE_SUMMARY"
echo "  timer_churn summary: $TIMER_CHURN_SUMMARY"
echo "  thread_pool_scaling summary: $THREAD_POOL_SCALING_SUMMARY"

if [[ ${#FAILED_STEPS[@]} -gt 0 ]]; then
  echo
  echo "One or more benchmark steps failed:" >&2
  for step in "${FAILED_STEPS[@]}"; do
    echo "  - $step" >&2
  done
  exit 1
fi
