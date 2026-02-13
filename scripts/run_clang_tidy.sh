#!/usr/bin/env bash

set -euo pipefail

BUILD_DIR="build-tidy"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
CLEAN=false
RUN_TESTS=false

print_help() {
  cat << 'EOF'
Usage: ./scripts/run_clang_tidy.sh [OPTIONS]

Options:
  --build-dir DIR   Build directory (default: build-tidy)
  --jobs N          Parallel build jobs (default: auto-detected)
  --clean           Remove build directory before configuring
  --run-tests       Run ctest after build
  -h, --help        Show this help
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
    --clean)
      CLEAN=true
      shift
      ;;
    --run-tests)
      RUN_TESTS=true
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

if ! command -v clang-tidy >/dev/null 2>&1; then
  echo "[ERROR] clang-tidy is required" >&2
  exit 1
fi

if [[ "$CLEAN" == true ]]; then
  rm -rf "$BUILD_DIR"
fi

cmake -S . -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DIOCORO_BUILD_TESTS=ON \
  -DIOCORO_BUILD_EXAMPLES=ON \
  -DIOCORO_BUILD_BENCHMARKS=OFF \
  -DIOCORO_ENABLE_CLANG_TIDY=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build "$BUILD_DIR" -j "$JOBS"

if [[ "$RUN_TESTS" == true ]]; then
  ctest --test-dir "$BUILD_DIR" --output-on-failure -j "$JOBS"
fi

echo "[INFO] clang-tidy check completed"
