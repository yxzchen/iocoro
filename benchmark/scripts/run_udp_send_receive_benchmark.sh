#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

exec "$SCRIPT_DIR/run_ratio_suite.sh" \
  --suite-name "udp send/receive benchmark" \
  --usage-name "benchmark/scripts/run_udp_send_receive_benchmark.sh" \
  --scenario-fields "sessions,msgs,msg_bytes" \
  --scenario-format "sessions:msgs:msg_bytes tuples" \
  --scenarios-default "1:20000:64,8:8000:64,32:2000:64,8:4000:1024,32:1000:4096" \
  --iocoro-target "iocoro_udp_send_receive" \
  --asio-target "asio_udp_send_receive" \
  --metric-name "pps" \
  --ratio-mode "direct" \
  --ratio-field "ratio_vs_asio" \
  --run-timeout-default 120 \
  "$@"
