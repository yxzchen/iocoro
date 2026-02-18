# Contributing to iocoro

This document is for contributors working on implementation, tests, benchmarks, and CI.

## Prerequisites

- Linux
- CMake `>= 3.15`
- GCC and/or Clang with C++20 coroutine support
- Ninja (recommended)
- `libgtest-dev` for tests
- Python 3 (for helper scripts)
- Optional:
  - `pre-commit`
  - `clang-tidy`
  - `gcovr`
  - `liburing-dev` (for io_uring backend work)

## Repository Layout

- Public headers: `include/iocoro/`
- Tests: `test/`
- Examples: `examples/`
- Benchmarks: `benchmark/`
- Developer scripts: `scripts/`
- CI workflows: `.github/workflows/`

## Build and Test

### Preferred helper script

```bash
./scripts/build.sh -c -t
```

Useful variants:

```bash
./scripts/build.sh -c -r
./scripts/build.sh -c -t --asan
./scripts/build.sh -c -t --ubsan
./scripts/build.sh -c -t --tsan
```

### Raw CMake flow

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DIOCORO_BUILD_TESTS=ON \
  -DIOCORO_BUILD_EXAMPLES=ON \
  -DIOCORO_BUILD_BENCHMARKS=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure -j
```

## Quality Gates

### Pre-commit

```bash
pre-commit run --all-files
```

Current hooks include formatting and basic safety checks.

### Clang-tidy

```bash
./scripts/run_clang_tidy.sh --clean --build-dir build-tidy
```

- Configuration: `.clang-tidy`
- CI treats enabled checks as errors.

### Coverage

```bash
./scripts/run_coverage.sh --clean --build-dir build-coverage --min-line 70
```

- Current gate: line coverage must be strictly greater than 70%.

## Performance Workflow

Unified benchmark runner:

```bash
./benchmark/scripts/run_all_perf_benchmarks.sh \
  --build-dir build \
  --iterations 3 \
  --warmup 1 \
  --timeout-sec 180
```

For benchmark suites and report schemas, see `benchmark/README.md`.

## Backend Notes

- Default backend is epoll.
- To test io_uring path:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DIOCORO_ENABLE_URING=ON \
  -DIOCORO_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure -j
```

## Coding and Testing Expectations

- Keep behavior changes covered by tests in `test/`.
- Prefer targeted tests first, then run broader suites.
- Do not mix unrelated refactors with bug fixes in one commit.
- Keep public API changes documented in headers and tests.

## Commit Guidance

- Keep commits focused and atomic.
- Use short, explicit subjects (project convention):
  - `fix: ...`
  - `feat: ...`
  - `perf: ...`
  - `chore: ...`

## CI Alignment

Before opening PR, run at least:

```bash
pre-commit run --all-files
./scripts/run_clang_tidy.sh --build-dir build-tidy
./scripts/build.sh -c -t
```

If your change touches performance/bench code, also run the benchmark gate script.
