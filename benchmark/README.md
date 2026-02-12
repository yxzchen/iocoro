# Benchmarks

## TCP roundtrip baseline runner

Use `run_tcp_roundtrip_baseline.sh` to run `iocoro_tcp_roundtrip` and `asio_tcp_roundtrip`
across fixed scenarios and aggregate median `rps`.

Example:

```bash
./benchmark/run_tcp_roundtrip_baseline.sh \
  --build-dir build \
  --iterations 5 \
  --warmup 1 \
  --baseline benchmark/baseline/tcp_roundtrip_thresholds.txt \
  --report benchmark/perf_report.json
```

## Baseline threshold format

File: `benchmark/baseline/tcp_roundtrip_thresholds.txt`

```
# sessions msgs min_ratio_vs_asio
1 5000 0.60
8 2000 0.72
32 500 0.72
```

The gate compares `iocoro_rps_median / asio_rps_median` against `min_ratio_vs_asio`.
