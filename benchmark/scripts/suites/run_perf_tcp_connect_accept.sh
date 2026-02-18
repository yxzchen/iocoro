#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

exec "$SCRIPT_DIR/../run_perf_ratio_suite.sh" \
  --suite-name "tcp_connect_accept benchmark suite" \
  --usage-name "benchmark/scripts/suites/run_perf_tcp_connect_accept.sh" \
  --scenario-fields "connections" \
  --scenario-format "connection counts" \
  --scenarios-default "1000,2000,3000" \
  --iocoro-target "iocoro_tcp_connect_accept" \
  --asio-target "asio_tcp_connect_accept" \
  --metric-name "cps" \
  --ratio-mode "direct" \
  --ratio-field "ratio_vs_asio" \
  --run-timeout-default 120 \
  "$@"
