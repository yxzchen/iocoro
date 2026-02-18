# Benchmarks

This directory uses one unified entrypoint:

- `benchmark/scripts/run_perf_benchmarks.sh`

Directory layout:

- `benchmark/cases/`: benchmark binaries (`iocoro_*` and `asio_*` pairs).
- `benchmark/scripts/suites/`: per-suite runner scripts.
- `benchmark/scripts/run_perf_ratio_suite.sh`: shared benchmark runner logic.
- `benchmark/scripts/validate_benchmark_report.py`: validation utility.
- `benchmark/schemas/`: JSON schemas for each suite report.
- `benchmark/conf/`: per-suite config files (`ITERATIONS`, `WARMUP`, `TIMEOUT_SEC`, `SCENARIO_ROWS`).
- `benchmark/reports/`: generated reports and summaries.

## Quick start

```bash
./benchmark/scripts/run_perf_benchmarks.sh \
  --build-dir build \
  --iterations 3 \
  --warmup 1 \
  --timeout-sec 180
```

Default behavior:

- Runs all benchmark suites.
- Runs report schema validation.

## Common options

- `--no-schema-validate`: skip JSON schema validation.
- `--timeout-sec N`: set per-process timeout for all suites.
- `--*-scenarios ...`: override suite scenario matrix.

## Suite Config Files

Each suite reads defaults from `benchmark/conf/<suite_id>.conf`.

Config keys:

- `ITERATIONS`: measured runs per scenario
- `WARMUP`: warmup runs per scenario
- `TIMEOUT_SEC`: per-process timeout in seconds
- `SCENARIO_ROWS`: suite-specific scenario list with explicit field names

Example:

```bash
SCENARIO_ROWS=(
  "sessions=1 msgs=5000 msg_bytes=64"
  "sessions=8 msgs=1000 msg_bytes=1024"
)
```

## Outputs

Reports and summaries are written under `benchmark/reports/`.

File naming is unambiguous and suite-based:

- Report: `benchmark/reports/<suite_id>.report.json`
- Summary: `benchmark/reports/<suite_id>.summary.txt`
- Schema: `benchmark/schemas/<suite_id>.schema.json`

Suite IDs:

- `tcp_roundtrip`
- `tcp_latency`
- `tcp_connect_accept`
- `tcp_throughput`
- `udp_send_receive`
- `timer_churn`
- `thread_pool_scaling`
