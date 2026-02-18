#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

exec "$SCRIPT_DIR/run_ratio_suite.sh" \
  --suite-name "tcp throughput benchmark" \
  --usage-name "benchmark/scripts/run_tcp_throughput_benchmark.sh" \
  --scenario-fields "sessions,bytes_per_session,chunk_bytes" \
  --scenario-format "sessions:bytes_per_session:chunk_bytes tuples" \
  --scenarios-default "1:16777216:16384,8:4194304:16384,32:1048576:8192" \
  --iocoro-target "iocoro_tcp_throughput" \
  --asio-target "asio_tcp_throughput" \
  --metric-name "throughput_mib_s" \
  --ratio-mode "direct" \
  --ratio-field "ratio_vs_asio" \
  --run-timeout-default 120 \
  "$@"
