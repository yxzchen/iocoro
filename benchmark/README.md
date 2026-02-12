# Benchmarks

Directory layout:

- `apps/`: benchmark binaries source files.
- `scripts/`: benchmark runners and report validator.
- `scripts/common.sh`: shared shell helpers used by all runners.
- `baselines/`: regression gate thresholds.
- `schemas/`: JSON schemas for benchmark reports.
- `reports/`: generated reports (runtime output, not committed).

## Unified runner

Run all benchmark suites with one command:

```bash
./benchmark/scripts/run_all_perf_benchmarks.sh \
  --build-dir build \
  --iterations 3 \
  --warmup 1 \
  --timeout-sec 180
```

The unified runner executes:
- `scripts/run_tcp_roundtrip_baseline.sh`
- `scripts/run_tcp_latency_baseline.sh`
- `scripts/run_tcp_connect_accept_baseline.sh`
- `scripts/run_tcp_throughput_baseline.sh`
- `scripts/run_udp_send_receive_baseline.sh`
- `scripts/run_timer_churn_baseline.sh`
- `scripts/run_thread_pool_scaling_baseline.sh`
- `scripts/validate_perf_report.py` for all reports

Timeout options:
- `--timeout-sec` applies to all suites.
- `--roundtrip-timeout-sec`, `--latency-timeout-sec`, `--connect-timeout-sec`, `--throughput-timeout-sec`, `--udp-timeout-sec`, `--timer-timeout-sec`, and `--threadpool-timeout-sec` can override per-suite.

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

## TCP latency baseline runner

Use `run_tcp_latency_baseline.sh` to run `iocoro_tcp_latency` and `asio_tcp_latency`
across fixed scenarios and aggregate median `p50/p95/p99` latency.

Example:

```bash
./benchmark/scripts/run_tcp_latency_baseline.sh \
  --build-dir build \
  --iterations 5 \
  --warmup 1 \
  --run-timeout-sec 90 \
  --baseline benchmark/baselines/tcp_latency_thresholds.txt \
  --report benchmark/reports/latency_report.json
```

Latency scenarios are `sessions:msgs:msg_bytes`.

## TCP latency threshold format

File: `benchmark/baselines/tcp_latency_thresholds.txt`

```
# sessions msgs msg_bytes min_ratio_vs_asio_p95
1 5000 64 0.40
8 1000 64 0.65
8 500 1024 0.65
```

The gate compares `asio_p95_us_median / iocoro_p95_us_median` against `min_ratio_vs_asio_p95`.

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

## TCP throughput baseline runner

Use `run_tcp_throughput_baseline.sh` to run `iocoro_tcp_throughput` and
`asio_tcp_throughput` across session/transfer-size scenarios and aggregate median `throughput_mib_s`.

Example:

```bash
./benchmark/scripts/run_tcp_throughput_baseline.sh \
  --build-dir build \
  --iterations 5 \
  --warmup 1 \
  --run-timeout-sec 120 \
  --baseline benchmark/baselines/tcp_throughput_thresholds.txt \
  --report benchmark/reports/throughput_report.json
```

Throughput scenarios are `sessions:bytes_per_session:chunk_bytes`.

## TCP throughput threshold format

File: `benchmark/baselines/tcp_throughput_thresholds.txt`

```
# sessions bytes_per_session chunk_bytes min_ratio_vs_asio
1 16777216 16384 0.55
8 4194304 16384 0.60
32 1048576 8192 0.60
```

The gate compares
`iocoro_throughput_mib_s_median / asio_throughput_mib_s_median` against `min_ratio_vs_asio`.

## UDP send/receive baseline runner

Use `run_udp_send_receive_baseline.sh` to run `iocoro_udp_send_receive` and
`asio_udp_send_receive` across datagram scenarios and aggregate median `pps`.

Example:

```bash
./benchmark/scripts/run_udp_send_receive_baseline.sh \
  --build-dir build \
  --iterations 5 \
  --warmup 1 \
  --run-timeout-sec 120 \
  --baseline benchmark/baselines/udp_send_receive_thresholds.txt \
  --report benchmark/reports/udp_report.json
```

UDP scenarios are `sessions:msgs:msg_bytes`.

## UDP send/receive threshold format

File: `benchmark/baselines/udp_send_receive_thresholds.txt`

```
# sessions msgs msg_bytes min_ratio_vs_asio
1 20000 64 0.55
8 8000 64 0.60
32 2000 64 0.60
8 4000 1024 0.60
32 1000 4096 0.55
```

The gate compares `iocoro_pps_median / asio_pps_median` against `min_ratio_vs_asio`.

## Timer churn baseline runner

Use `run_timer_churn_baseline.sh` to run `iocoro_timer_churn` and
`asio_timer_churn` across scheduler-load scenarios and aggregate median `ops_s`.

Example:

```bash
./benchmark/scripts/run_timer_churn_baseline.sh \
  --build-dir build \
  --iterations 5 \
  --warmup 1 \
  --run-timeout-sec 120 \
  --baseline benchmark/baselines/timer_churn_thresholds.txt \
  --report benchmark/reports/timer_report.json
```

Timer scenarios are `sessions:waits`.

## Timer churn threshold format

File: `benchmark/baselines/timer_churn_thresholds.txt`

```
# sessions waits min_ratio_vs_asio
1 200000 0.65
8 80000 0.70
32 20000 0.70
```

The gate compares `iocoro_ops_s_median / asio_ops_s_median` against `min_ratio_vs_asio`.

## Thread pool scaling baseline runner

Use `run_thread_pool_scaling_baseline.sh` to run `iocoro_thread_pool_scaling` and
`asio_thread_pool_scaling` across worker-count scenarios and aggregate median `ops_s`.

Example:

```bash
./benchmark/scripts/run_thread_pool_scaling_baseline.sh \
  --build-dir build \
  --iterations 5 \
  --warmup 1 \
  --run-timeout-sec 120 \
  --baseline benchmark/baselines/thread_pool_scaling_thresholds.txt \
  --report benchmark/reports/thread_pool_report.json
```

Thread pool scenarios are `workers:tasks`.

## Thread pool scaling threshold format

File: `benchmark/baselines/thread_pool_scaling_thresholds.txt`

```
# workers tasks min_ratio_vs_asio
1 200000 0.85
2 400000 0.75
4 800000 0.65
```

The gate compares `iocoro_ops_s_median / asio_ops_s_median` against `min_ratio_vs_asio`.

## Report schemas

Schema files:
- `benchmark/schemas/perf_report.schema.json`
- `benchmark/schemas/latency_report.schema.json`
- `benchmark/schemas/connect_accept_report.schema.json`
- `benchmark/schemas/throughput_report.schema.json`
- `benchmark/schemas/udp_report.schema.json`
- `benchmark/schemas/timer_report.schema.json`
- `benchmark/schemas/thread_pool_report.schema.json`

Validate a generated report:

```bash
python3 benchmark/scripts/validate_perf_report.py \
  --schema benchmark/schemas/perf_report.schema.json \
  --report benchmark/reports/perf_report.json
```

```bash
python3 benchmark/scripts/validate_perf_report.py \
  --schema benchmark/schemas/latency_report.schema.json \
  --report benchmark/reports/latency_report.json
```

```bash
python3 benchmark/scripts/validate_perf_report.py \
  --schema benchmark/schemas/connect_accept_report.schema.json \
  --report benchmark/reports/connect_accept_report.json
```

```bash
python3 benchmark/scripts/validate_perf_report.py \
  --schema benchmark/schemas/throughput_report.schema.json \
  --report benchmark/reports/throughput_report.json
```

```bash
python3 benchmark/scripts/validate_perf_report.py \
  --schema benchmark/schemas/udp_report.schema.json \
  --report benchmark/reports/udp_report.json
```

```bash
python3 benchmark/scripts/validate_perf_report.py \
  --schema benchmark/schemas/timer_report.schema.json \
  --report benchmark/reports/timer_report.json
```

```bash
python3 benchmark/scripts/validate_perf_report.py \
  --schema benchmark/schemas/thread_pool_report.schema.json \
  --report benchmark/reports/thread_pool_report.json
```
