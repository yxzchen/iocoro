#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
PROJECT_DIR="$(cd -- "${BENCH_DIR}/.." && pwd)"

BUILD_DIR="$PROJECT_DIR/build"
ITERATIONS=5
WARMUP=1
ROUNDTRIP_SCENARIOS=""
CONNECT_SCENARIOS=""
ROUNDTRIP_TIMEOUT_SEC=60
CONNECT_TIMEOUT_SEC=120
ENABLE_BASELINE=true
ENABLE_SCHEMA_VALIDATE=true

ROUNDTRIP_BASELINE="$PROJECT_DIR/benchmark/baselines/tcp_roundtrip_thresholds.txt"
CONNECT_BASELINE="$PROJECT_DIR/benchmark/baselines/tcp_connect_accept_thresholds.txt"
ROUNDTRIP_REPORT="$PROJECT_DIR/benchmark/reports/perf_report.json"
CONNECT_REPORT="$PROJECT_DIR/benchmark/reports/connect_accept_report.json"
ROUNDTRIP_SUMMARY="$PROJECT_DIR/benchmark/reports/perf_summary.txt"
CONNECT_SUMMARY="$PROJECT_DIR/benchmark/reports/connect_accept_summary.txt"

usage() {
  cat <<'EOF'
Usage: benchmark/scripts/run_all_perf_benchmarks.sh [options]

Run all benchmark suites:
- TCP roundtrip
- TCP connect/accept

Options:
  --build-dir DIR             CMake build dir (default: ./build)
  --iterations N              Measured runs per scenario (default: 5)
  --warmup N                  Warmup runs per scenario (default: 1)
  --roundtrip-scenarios LIST  Override roundtrip scenarios (sessions:msgs:msg_bytes tuples)
  --connect-scenarios LIST    Override connect scenarios (connection counts)
  --roundtrip-timeout-sec N   Per-process timeout for roundtrip suite (default: 60, 0=disable)
  --connect-timeout-sec N     Per-process timeout for connect suite (default: 120, 0=disable)
  --no-baseline               Disable regression gate (no threshold checks)
  --no-schema-validate        Skip JSON schema validation
  --roundtrip-report FILE     Roundtrip report path (default: benchmark/reports/perf_report.json)
  --connect-report FILE       Connect/accept report path (default: benchmark/reports/connect_accept_report.json)
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
    --roundtrip-timeout-sec)
      ROUNDTRIP_TIMEOUT_SEC="$2"
      shift 2
      ;;
    --connect-timeout-sec)
      CONNECT_TIMEOUT_SEC="$2"
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

for n in "$ITERATIONS" "$WARMUP" "$ROUNDTRIP_TIMEOUT_SEC" "$CONNECT_TIMEOUT_SEC"; do
  if ! [[ "$n" =~ ^[0-9]+$ ]]; then
    echo "Numeric options must be non-negative integers" >&2
    exit 1
  fi
done
if [[ "$ITERATIONS" -le 0 ]]; then
  echo "--iterations must be > 0" >&2
  exit 1
fi

if [[ "$BUILD_DIR" != /* ]]; then
  BUILD_DIR="$PROJECT_DIR/$BUILD_DIR"
fi
if [[ "$ROUNDTRIP_REPORT" != /* ]]; then
  ROUNDTRIP_REPORT="$PROJECT_DIR/$ROUNDTRIP_REPORT"
fi
if [[ "$CONNECT_REPORT" != /* ]]; then
  CONNECT_REPORT="$PROJECT_DIR/$CONNECT_REPORT"
fi

ROUNDTRIP_SUMMARY="$(dirname -- "$ROUNDTRIP_REPORT")/perf_summary.txt"
CONNECT_SUMMARY="$(dirname -- "$CONNECT_REPORT")/connect_accept_summary.txt"
mkdir -p "$(dirname -- "$ROUNDTRIP_REPORT")"
mkdir -p "$(dirname -- "$CONNECT_REPORT")"

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

echo "Running full benchmark suite"
echo "  build_dir: $BUILD_DIR"
echo "  iterations: $ITERATIONS, warmup: $WARMUP"
echo "  baseline_gate: $ENABLE_BASELINE"
echo "  schema_validate: $ENABLE_SCHEMA_VALIDATE"
echo

"${roundtrip_cmd[@]}" | tee "$ROUNDTRIP_SUMMARY"

"${connect_cmd[@]}" | tee "$CONNECT_SUMMARY"

if [[ "$ENABLE_SCHEMA_VALIDATE" == true ]]; then
  python3 "$SCRIPT_DIR/validate_perf_report.py" \
    --schema "$PROJECT_DIR/benchmark/schemas/perf_report.schema.json" \
    --report "$ROUNDTRIP_REPORT"

  python3 "$SCRIPT_DIR/validate_perf_report.py" \
    --schema "$PROJECT_DIR/benchmark/schemas/connect_accept_report.schema.json" \
    --report "$CONNECT_REPORT"
fi

echo
echo "Benchmark suite completed"
echo "  roundtrip report: $ROUNDTRIP_REPORT"
echo "  connect report  : $CONNECT_REPORT"
echo "  roundtrip summary: $ROUNDTRIP_SUMMARY"
echo "  connect summary  : $CONNECT_SUMMARY"
