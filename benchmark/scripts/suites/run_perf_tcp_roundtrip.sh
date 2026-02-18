#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

exec "$SCRIPT_DIR/../run_perf_ratio_suite.sh" \
  --suite-name "tcp_roundtrip benchmark suite" \
  --usage-name "benchmark/scripts/suites/run_perf_tcp_roundtrip.sh" \
  --scenario-fields "sessions,msgs,msg_bytes" \
  --scenario-format "sessions:msgs:msg_bytes tuples" \
  --scenarios-default "1:5000:64,8:2000:64,32:500:64,8:1000:1024,32:200:4096" \
  --iocoro-target "iocoro_tcp_roundtrip" \
  --asio-target "asio_tcp_roundtrip" \
  --metric-names "rps" \
  --primary-metric "rps" \
  --ratio-mode "direct" \
  --ratio-field "ratio_vs_asio" \
  --ratio-label "iocoro/asio" \
  --run-timeout-default 60 \
  "$@"
