# Benchmarks

This directory uses one unified entrypoint:

- `benchmark/scripts/run_all_perf_benchmarks.sh`

## Quick start

```bash
./benchmark/scripts/run_all_perf_benchmarks.sh \
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

## Outputs

Reports and summaries are written under `benchmark/reports/`.

Key outputs:

- `benchmark/reports/perf_report.json`
- `benchmark/reports/latency_report.json`
- `benchmark/reports/connect_accept_report.json`
- `benchmark/reports/throughput_report.json`
- `benchmark/reports/udp_report.json`
- `benchmark/reports/timer_report.json`
- `benchmark/reports/thread_pool_report.json`
