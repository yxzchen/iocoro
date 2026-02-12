#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

exec "$SCRIPT_DIR/run_ratio_baseline_suite.sh" \
  --suite-name "tcp latency benchmark" \
  --usage-name "benchmark/scripts/run_tcp_latency_baseline.sh" \
  --scenario-fields "sessions,msgs,msg_bytes" \
  --scenario-format "sessions:msgs:msg_bytes tuples" \
  --scenarios-default "1:5000:64,8:1000:64,8:500:1024" \
  --baseline-description "Threshold file format: \"sessions msgs msg_bytes min_ratio\" where ratio is asio_p95 / iocoro_p95" \
  --iocoro-target "iocoro_tcp_latency" \
  --asio-target "asio_tcp_latency" \
  --metric-names "p50_us,p95_us,p99_us" \
  --primary-metric "p95_us" \
  --ratio-mode "inverse" \
  --ratio-field "ratio_vs_asio_p95" \
  --ratio-label "asio_p95 / iocoro_p95" \
  --run-timeout-default 90 \
  "$@"
