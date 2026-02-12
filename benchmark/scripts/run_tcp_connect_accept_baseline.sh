#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

exec "$SCRIPT_DIR/run_ratio_baseline_suite.sh" \
  --suite-name "tcp connect/accept benchmark" \
  --usage-name "benchmark/scripts/run_tcp_connect_accept_baseline.sh" \
  --scenario-fields "connections" \
  --scenario-format "connection counts" \
  --scenarios-default "1000,2000,3000" \
  --baseline-description "Threshold file format: \"connections min_ratio\"" \
  --iocoro-target "iocoro_tcp_connect_accept" \
  --asio-target "asio_tcp_connect_accept" \
  --metric-name "cps" \
  --ratio-mode "direct" \
  --ratio-field "ratio_vs_asio" \
  --run-timeout-default 120 \
  "$@"
