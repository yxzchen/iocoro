#!/usr/bin/env bash

set -euo pipefail

BUILD_DIR="build-coverage"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
CLEAN=false
MIN_LINE_COVERAGE=70.0

print_help() {
  cat << 'EOF'
Usage: ./scripts/run_coverage.sh [OPTIONS]

Options:
  --build-dir DIR    Build directory (default: build-coverage)
  --jobs N           Parallel build/test jobs (default: auto-detected)
  --min-line PCT     Minimum line coverage percent, must be exceeded (default: 70.0)
  --clean            Remove build directory before configuring
  -h, --help         Show this help

Artifacts:
  <build-dir>/coverage/summary.txt
  <build-dir>/coverage/summary.json
  <build-dir>/coverage/coverage.xml
  <build-dir>/coverage/index.html
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --jobs)
      JOBS="$2"
      shift 2
      ;;
    --min-line)
      MIN_LINE_COVERAGE="$2"
      shift 2
      ;;
    --clean)
      CLEAN=true
      shift
      ;;
    -h|--help)
      print_help
      exit 0
      ;;
    *)
      echo "[ERROR] Unknown option: $1" >&2
      print_help
      exit 1
      ;;
  esac
done

if ! command -v cmake >/dev/null 2>&1; then
  echo "[ERROR] cmake is required" >&2
  exit 1
fi

if ! command -v gcovr >/dev/null 2>&1; then
  echo "[ERROR] gcovr is required (apt install gcovr)" >&2
  exit 1
fi

if [[ "$CLEAN" == true ]]; then
  rm -rf "$BUILD_DIR"
fi

cmake -S . -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DIOCORO_BUILD_TESTS=ON \
  -DIOCORO_BUILD_EXAMPLES=OFF \
  -DIOCORO_BUILD_BENCHMARKS=OFF \
  -DIOCORO_ENABLE_COVERAGE=ON

cmake --build "$BUILD_DIR" -j "$JOBS"
ctest --test-dir "$BUILD_DIR" --output-on-failure -j "$JOBS"

OUT_DIR="$BUILD_DIR/coverage"
mkdir -p "$OUT_DIR"

COMMON_ARGS=(
  --root .
  --object-directory "$BUILD_DIR"
  --filter 'include/iocoro/.*'
  --exclude '.*/test/.*'
  --exclude '.*/benchmark/.*'
  --exclude '.*/examples/.*'
  --exclude '.*/build.*/.*'
  --gcov-ignore-parse-errors negative_hits.warn_once_per_file
)

gcovr "${COMMON_ARGS[@]}" --txt "$OUT_DIR/summary.txt" --print-summary
gcovr "${COMMON_ARGS[@]}" --json-summary-pretty --json-summary "$OUT_DIR/summary.json"
gcovr "${COMMON_ARGS[@]}" --xml-pretty --xml "$OUT_DIR/coverage.xml"
gcovr "${COMMON_ARGS[@]}" --html-details "$OUT_DIR/index.html"

python3 - "$OUT_DIR/summary.json" <<'PY'
import json
import pathlib
import sys

summary_path = pathlib.Path(sys.argv[1])
with summary_path.open("r", encoding="utf-8") as f:
    data = json.load(f)

line = data.get("line_percent", 0.0)
branch = data.get("branch_percent", 0.0)
func = data.get("function_percent", 0.0)
print(f"[INFO] Coverage summary: line={line:.2f}% branch={branch:.2f}% function={func:.2f}%")
PY

python3 - "$OUT_DIR/summary.json" "$MIN_LINE_COVERAGE" <<'PY'
import json
import pathlib
import sys

summary_path = pathlib.Path(sys.argv[1])
min_line = float(sys.argv[2])
with summary_path.open("r", encoding="utf-8") as f:
    data = json.load(f)

line = float(data.get("line_percent", 0.0))
if line <= min_line:
    print(
        f"[ERROR] Line coverage gate failed: {line:.2f}% is not greater than {min_line:.2f}%",
        file=sys.stderr,
    )
    sys.exit(1)
print(f"[INFO] Line coverage gate passed: {line:.2f}% > {min_line:.2f}%")
PY

echo "[INFO] Coverage reports written to: $OUT_DIR"
