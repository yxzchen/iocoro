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
- Enabled checks listed in `.clang-tidy` are treated as errors.

### Coverage

```bash
./scripts/run_coverage.sh --clean --build-dir build-coverage --min-line 70
```

- Current gate: line coverage must be strictly greater than 70%.

## Performance Workflow

Unified benchmark runner:

```bash
./benchmark/scripts/run_perf_benchmarks.sh \
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
- Follow the repository's Conventional Commits style with a short optional scope.
- Prefer subjects like:
  - `fix(socket): ...`
  - `refactor(executor): ...`
  - `test(io_context): ...`
  - `docs(architecture): ...`
  - `perf: ...`
  - `chore(ci): ...`
  - `revert(...): ...`

## CI Alignment

Before opening PR, run at least:

```bash
pre-commit run --all-files
./scripts/build.sh -c -t
```

Also run these when relevant:

- `./scripts/run_clang_tidy.sh --clean --build-dir build-tidy` for code paths touched by lint/static-analysis-sensitive changes
- io_uring configure/build/test flow if you touched backend or reactor behavior
- `./scripts/run_coverage.sh --clean --build-dir build-coverage --min-line 70` if you are validating coverage-sensitive changes
- `./benchmark/scripts/run_perf_benchmarks.sh ...` if your change touches performance or benchmark code
