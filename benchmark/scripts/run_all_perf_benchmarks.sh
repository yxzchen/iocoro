#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
PROJECT_DIR="$(cd -- "${BENCH_DIR}/.." && pwd)"
source "$SCRIPT_DIR/common.sh"

BUILD_DIR="$PROJECT_DIR/build"
ITERATIONS=5
WARMUP=1
ROUNDTRIP_SCENARIOS=""
CONNECT_SCENARIOS=""
THROUGHPUT_SCENARIOS=""
UDP_SCENARIOS=""
TIMER_SCENARIOS=""
ROUNDTRIP_TIMEOUT_SEC=60
CONNECT_TIMEOUT_SEC=120
THROUGHPUT_TIMEOUT_SEC=120
UDP_TIMEOUT_SEC=120
TIMER_TIMEOUT_SEC=120
TIMEOUT_SEC=""
ROUNDTRIP_TIMEOUT_SET=false
CONNECT_TIMEOUT_SET=false
THROUGHPUT_TIMEOUT_SET=false
UDP_TIMEOUT_SET=false
TIMER_TIMEOUT_SET=false
ENABLE_BASELINE=true
ENABLE_SCHEMA_VALIDATE=true

ROUNDTRIP_BASELINE="$PROJECT_DIR/benchmark/baselines/tcp_roundtrip_thresholds.txt"
CONNECT_BASELINE="$PROJECT_DIR/benchmark/baselines/tcp_connect_accept_thresholds.txt"
THROUGHPUT_BASELINE="$PROJECT_DIR/benchmark/baselines/tcp_throughput_thresholds.txt"
UDP_BASELINE="$PROJECT_DIR/benchmark/baselines/udp_send_receive_thresholds.txt"
TIMER_BASELINE="$PROJECT_DIR/benchmark/baselines/timer_churn_thresholds.txt"
ROUNDTRIP_REPORT="$PROJECT_DIR/benchmark/reports/perf_report.json"
CONNECT_REPORT="$PROJECT_DIR/benchmark/reports/connect_accept_report.json"
THROUGHPUT_REPORT="$PROJECT_DIR/benchmark/reports/throughput_report.json"
UDP_REPORT="$PROJECT_DIR/benchmark/reports/udp_report.json"
TIMER_REPORT="$PROJECT_DIR/benchmark/reports/timer_report.json"
ROUNDTRIP_SUMMARY="$PROJECT_DIR/benchmark/reports/perf_summary.txt"
CONNECT_SUMMARY="$PROJECT_DIR/benchmark/reports/connect_accept_summary.txt"
THROUGHPUT_SUMMARY="$PROJECT_DIR/benchmark/reports/throughput_summary.txt"
UDP_SUMMARY="$PROJECT_DIR/benchmark/reports/udp_summary.txt"
TIMER_SUMMARY="$PROJECT_DIR/benchmark/reports/timer_summary.txt"

usage() {
  cat <<'EOF'
Usage: benchmark/scripts/run_all_perf_benchmarks.sh [options]

Run all benchmark suites:
- TCP roundtrip
- TCP connect/accept
- TCP throughput
- UDP send/receive
- Timer churn

Options:
  --build-dir DIR             CMake build dir (default: ./build)
  --iterations N              Measured runs per scenario (default: 5)
  --warmup N                  Warmup runs per scenario (default: 1)
  --roundtrip-scenarios LIST  Override roundtrip scenarios (sessions:msgs:msg_bytes tuples)
  --connect-scenarios LIST    Override connect scenarios (connection counts)
  --throughput-scenarios LIST Override throughput scenarios (sessions:bytes_per_session:chunk_bytes tuples)
  --udp-scenarios LIST        Override UDP scenarios (sessions:msgs:msg_bytes tuples)
  --timer-scenarios LIST      Override timer scenarios (sessions:waits tuples)
  --timeout-sec N             Per-process timeout for all suites (0=disable)
  --roundtrip-timeout-sec N   Per-process timeout override for roundtrip suite
  --connect-timeout-sec N     Per-process timeout override for connect suite
  --throughput-timeout-sec N  Per-process timeout override for throughput suite
  --udp-timeout-sec N         Per-process timeout override for UDP suite
  --timer-timeout-sec N       Per-process timeout override for timer suite
  --no-baseline               Disable regression gate (no threshold checks)
  --no-schema-validate        Skip JSON schema validation
  --roundtrip-report FILE     Roundtrip report path (default: benchmark/reports/perf_report.json)
  --connect-report FILE       Connect/accept report path (default: benchmark/reports/connect_accept_report.json)
  --throughput-report FILE    Throughput report path (default: benchmark/reports/throughput_report.json)
  --udp-report FILE           UDP report path (default: benchmark/reports/udp_report.json)
  --timer-report FILE         Timer report path (default: benchmark/reports/timer_report.json)
  -h, --help                  Show this help
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
    --roundtrip-scenarios)
      ROUNDTRIP_SCENARIOS="$2"
      shift 2
      ;;
    --connect-scenarios)
      CONNECT_SCENARIOS="$2"
      shift 2
      ;;
    --throughput-scenarios)
      THROUGHPUT_SCENARIOS="$2"
      shift 2
      ;;
    --udp-scenarios)
      UDP_SCENARIOS="$2"
      shift 2
      ;;
    --timer-scenarios)
      TIMER_SCENARIOS="$2"
      shift 2
      ;;
    --roundtrip-timeout-sec)
      ROUNDTRIP_TIMEOUT_SEC="$2"
      ROUNDTRIP_TIMEOUT_SET=true
      shift 2
      ;;
    --connect-timeout-sec)
      CONNECT_TIMEOUT_SEC="$2"
      CONNECT_TIMEOUT_SET=true
      shift 2
      ;;
    --throughput-timeout-sec)
      THROUGHPUT_TIMEOUT_SEC="$2"
      THROUGHPUT_TIMEOUT_SET=true
      shift 2
      ;;
    --udp-timeout-sec)
      UDP_TIMEOUT_SEC="$2"
      UDP_TIMEOUT_SET=true
      shift 2
      ;;
    --timer-timeout-sec)
      TIMER_TIMEOUT_SEC="$2"
      TIMER_TIMEOUT_SET=true
      shift 2
      ;;
    --timeout-sec)
      TIMEOUT_SEC="$2"
      shift 2
      ;;
    --no-baseline)
      ENABLE_BASELINE=false
      shift
      ;;
    --no-schema-validate)
      ENABLE_SCHEMA_VALIDATE=false
      shift
      ;;
    --roundtrip-report)
      ROUNDTRIP_REPORT="$2"
      shift 2
      ;;
    --connect-report)
      CONNECT_REPORT="$2"
      shift 2
      ;;
    --throughput-report)
      THROUGHPUT_REPORT="$2"
      shift 2
      ;;
    --udp-report)
      UDP_REPORT="$2"
      shift 2
      ;;
    --timer-report)
      TIMER_REPORT="$2"
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
if [[ -n "$TIMEOUT_SEC" ]]; then
  bench_require_non_negative_int "--timeout-sec" "$TIMEOUT_SEC"
fi
if [[ -n "$TIMEOUT_SEC" && "$ROUNDTRIP_TIMEOUT_SET" == false ]]; then
  ROUNDTRIP_TIMEOUT_SEC="$TIMEOUT_SEC"
fi
if [[ -n "$TIMEOUT_SEC" && "$CONNECT_TIMEOUT_SET" == false ]]; then
  CONNECT_TIMEOUT_SEC="$TIMEOUT_SEC"
fi
if [[ -n "$TIMEOUT_SEC" && "$THROUGHPUT_TIMEOUT_SET" == false ]]; then
  THROUGHPUT_TIMEOUT_SEC="$TIMEOUT_SEC"
fi
if [[ -n "$TIMEOUT_SEC" && "$UDP_TIMEOUT_SET" == false ]]; then
  UDP_TIMEOUT_SEC="$TIMEOUT_SEC"
fi
if [[ -n "$TIMEOUT_SEC" && "$TIMER_TIMEOUT_SET" == false ]]; then
  TIMER_TIMEOUT_SEC="$TIMEOUT_SEC"
fi
bench_require_non_negative_int "--roundtrip-timeout-sec" "$ROUNDTRIP_TIMEOUT_SEC"
bench_require_non_negative_int "--connect-timeout-sec" "$CONNECT_TIMEOUT_SEC"
bench_require_non_negative_int "--throughput-timeout-sec" "$THROUGHPUT_TIMEOUT_SEC"
bench_require_non_negative_int "--udp-timeout-sec" "$UDP_TIMEOUT_SEC"
bench_require_non_negative_int "--timer-timeout-sec" "$TIMER_TIMEOUT_SEC"

BUILD_DIR="$(bench_to_abs_path "$PROJECT_DIR" "$BUILD_DIR")"
ROUNDTRIP_REPORT="$(bench_to_abs_path "$PROJECT_DIR" "$ROUNDTRIP_REPORT")"
CONNECT_REPORT="$(bench_to_abs_path "$PROJECT_DIR" "$CONNECT_REPORT")"
THROUGHPUT_REPORT="$(bench_to_abs_path "$PROJECT_DIR" "$THROUGHPUT_REPORT")"
UDP_REPORT="$(bench_to_abs_path "$PROJECT_DIR" "$UDP_REPORT")"
TIMER_REPORT="$(bench_to_abs_path "$PROJECT_DIR" "$TIMER_REPORT")"

ROUNDTRIP_SUMMARY="$(dirname -- "$ROUNDTRIP_REPORT")/perf_summary.txt"
CONNECT_SUMMARY="$(dirname -- "$CONNECT_REPORT")/connect_accept_summary.txt"
THROUGHPUT_SUMMARY="$(dirname -- "$THROUGHPUT_REPORT")/throughput_summary.txt"
UDP_SUMMARY="$(dirname -- "$UDP_REPORT")/udp_summary.txt"
TIMER_SUMMARY="$(dirname -- "$TIMER_REPORT")/timer_summary.txt"
mkdir -p "$(dirname -- "$ROUNDTRIP_REPORT")"
mkdir -p "$(dirname -- "$CONNECT_REPORT")"
mkdir -p "$(dirname -- "$THROUGHPUT_REPORT")"
mkdir -p "$(dirname -- "$UDP_REPORT")"
mkdir -p "$(dirname -- "$TIMER_REPORT")"

roundtrip_cmd=(
  "$SCRIPT_DIR/run_tcp_roundtrip_baseline.sh"
  --build-dir "$BUILD_DIR"
  --iterations "$ITERATIONS"
  --warmup "$WARMUP"
  --run-timeout-sec "$ROUNDTRIP_TIMEOUT_SEC"
  --report "$ROUNDTRIP_REPORT"
)
if [[ -n "$ROUNDTRIP_SCENARIOS" ]]; then
  roundtrip_cmd+=(--scenarios "$ROUNDTRIP_SCENARIOS")
fi
if [[ "$ENABLE_BASELINE" == true ]]; then
  roundtrip_cmd+=(--baseline "$ROUNDTRIP_BASELINE")
fi

connect_cmd=(
  "$SCRIPT_DIR/run_tcp_connect_accept_baseline.sh"
  --build-dir "$BUILD_DIR"
  --iterations "$ITERATIONS"
  --warmup "$WARMUP"
  --run-timeout-sec "$CONNECT_TIMEOUT_SEC"
  --report "$CONNECT_REPORT"
)
if [[ -n "$CONNECT_SCENARIOS" ]]; then
  connect_cmd+=(--scenarios "$CONNECT_SCENARIOS")
fi
if [[ "$ENABLE_BASELINE" == true ]]; then
  connect_cmd+=(--baseline "$CONNECT_BASELINE")
fi

throughput_cmd=(
  "$SCRIPT_DIR/run_tcp_throughput_baseline.sh"
  --build-dir "$BUILD_DIR"
  --iterations "$ITERATIONS"
  --warmup "$WARMUP"
  --run-timeout-sec "$THROUGHPUT_TIMEOUT_SEC"
  --report "$THROUGHPUT_REPORT"
)
if [[ -n "$THROUGHPUT_SCENARIOS" ]]; then
  throughput_cmd+=(--scenarios "$THROUGHPUT_SCENARIOS")
fi
if [[ "$ENABLE_BASELINE" == true ]]; then
  throughput_cmd+=(--baseline "$THROUGHPUT_BASELINE")
fi

udp_cmd=(
  "$SCRIPT_DIR/run_udp_send_receive_baseline.sh"
  --build-dir "$BUILD_DIR"
  --iterations "$ITERATIONS"
  --warmup "$WARMUP"
  --run-timeout-sec "$UDP_TIMEOUT_SEC"
  --report "$UDP_REPORT"
)
if [[ -n "$UDP_SCENARIOS" ]]; then
  udp_cmd+=(--scenarios "$UDP_SCENARIOS")
fi
if [[ "$ENABLE_BASELINE" == true ]]; then
  udp_cmd+=(--baseline "$UDP_BASELINE")
fi

timer_cmd=(
  "$SCRIPT_DIR/run_timer_churn_baseline.sh"
  --build-dir "$BUILD_DIR"
  --iterations "$ITERATIONS"
  --warmup "$WARMUP"
  --run-timeout-sec "$TIMER_TIMEOUT_SEC"
  --report "$TIMER_REPORT"
)
if [[ -n "$TIMER_SCENARIOS" ]]; then
  timer_cmd+=(--scenarios "$TIMER_SCENARIOS")
fi
if [[ "$ENABLE_BASELINE" == true ]]; then
  timer_cmd+=(--baseline "$TIMER_BASELINE")
fi

echo "Running full benchmark suite"
echo "  build_dir: $BUILD_DIR"
echo "  iterations: $ITERATIONS, warmup: $WARMUP"
echo "  baseline_gate: $ENABLE_BASELINE"
echo "  schema_validate: $ENABLE_SCHEMA_VALIDATE"
echo

"${roundtrip_cmd[@]}" | tee "$ROUNDTRIP_SUMMARY"

"${connect_cmd[@]}" | tee "$CONNECT_SUMMARY"

"${throughput_cmd[@]}" | tee "$THROUGHPUT_SUMMARY"

"${udp_cmd[@]}" | tee "$UDP_SUMMARY"

"${timer_cmd[@]}" | tee "$TIMER_SUMMARY"

if [[ "$ENABLE_SCHEMA_VALIDATE" == true ]]; then
  python3 "$SCRIPT_DIR/validate_perf_report.py" \
    --schema "$PROJECT_DIR/benchmark/schemas/perf_report.schema.json" \
    --report "$ROUNDTRIP_REPORT"

  python3 "$SCRIPT_DIR/validate_perf_report.py" \
    --schema "$PROJECT_DIR/benchmark/schemas/connect_accept_report.schema.json" \
    --report "$CONNECT_REPORT"

  python3 "$SCRIPT_DIR/validate_perf_report.py" \
    --schema "$PROJECT_DIR/benchmark/schemas/throughput_report.schema.json" \
    --report "$THROUGHPUT_REPORT"

  python3 "$SCRIPT_DIR/validate_perf_report.py" \
    --schema "$PROJECT_DIR/benchmark/schemas/udp_report.schema.json" \
    --report "$UDP_REPORT"

  python3 "$SCRIPT_DIR/validate_perf_report.py" \
    --schema "$PROJECT_DIR/benchmark/schemas/timer_report.schema.json" \
    --report "$TIMER_REPORT"
fi

echo
echo "Benchmark suite completed"
echo "  roundtrip report: $ROUNDTRIP_REPORT"
echo "  connect report  : $CONNECT_REPORT"
echo "  throughput report: $THROUGHPUT_REPORT"
echo "  udp report      : $UDP_REPORT"
echo "  timer report    : $TIMER_REPORT"
echo "  roundtrip summary: $ROUNDTRIP_SUMMARY"
echo "  connect summary  : $CONNECT_SUMMARY"
echo "  throughput summary: $THROUGHPUT_SUMMARY"
echo "  udp summary      : $UDP_SUMMARY"
echo "  timer summary    : $TIMER_SUMMARY"
