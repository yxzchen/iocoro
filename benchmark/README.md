# Benchmarks

Directory layout:

- `apps/`: benchmark binaries source files.
- `scripts/`: benchmark runners and report validator.
- `baselines/`: regression gate thresholds.
- `schemas/`: JSON schemas for benchmark reports.
- `reports/`: generated reports (runtime output, not committed).

## TCP roundtrip baseline runner

Use `run_tcp_roundtrip_baseline.sh` to run `iocoro_tcp_roundtrip` and `asio_tcp_roundtrip`
across fixed scenarios and aggregate median `rps`.

Example:

```bash
./benchmark/scripts/run_tcp_roundtrip_baseline.sh \
  --build-dir build \
  --iterations 5 \
  --warmup 1 \
  --run-timeout-sec 60 \
  --baseline benchmark/baselines/tcp_roundtrip_thresholds.txt \
  --report benchmark/reports/perf_report.json
```

Roundtrip scenarios are `sessions:msgs:msg_bytes`.

## TCP roundtrip threshold format

File: `benchmark/baselines/tcp_roundtrip_thresholds.txt`

```
# sessions msgs msg_bytes min_ratio_vs_asio
1 5000 64 0.60
8 2000 64 0.72
32 500 64 0.72
8 1000 1024 0.70
32 200 4096 0.65
```

The gate compares `iocoro_rps_median / asio_rps_median` against `min_ratio_vs_asio`.

## TCP connect/accept baseline runner

Use `run_tcp_connect_accept_baseline.sh` to run `iocoro_tcp_connect_accept` and
`asio_tcp_connect_accept` across connection-count scenarios and aggregate median `cps`.

Example:

```bash
./benchmark/scripts/run_tcp_connect_accept_baseline.sh \
  --build-dir build \
  --iterations 5 \
  --warmup 1 \
  --run-timeout-sec 120 \
  --baseline benchmark/baselines/tcp_connect_accept_thresholds.txt \
  --report benchmark/reports/connect_accept_report.json
```

Connect/accept scenarios are plain connection counts.

## TCP connect/accept threshold format

File: `benchmark/baselines/tcp_connect_accept_thresholds.txt`

```
# connections min_ratio_vs_asio
1000 0.80
2000 0.80
3000 0.80
```

The gate compares `iocoro_cps_median / asio_cps_median` against `min_ratio_vs_asio`.

## Report schemas

Schema files:
- `benchmark/schemas/perf_report.schema.json`
- `benchmark/schemas/connect_accept_report.schema.json`

Validate a generated report:

```bash
python3 benchmark/scripts/validate_perf_report.py \
  --schema benchmark/schemas/perf_report.schema.json \
  --report benchmark/reports/perf_report.json
```

```bash
python3 benchmark/scripts/validate_perf_report.py \
  --schema benchmark/schemas/connect_accept_report.schema.json \
  --report benchmark/reports/connect_accept_report.json
```
