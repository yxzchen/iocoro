#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

exec "$SCRIPT_DIR/run_ratio_suite.sh" \
  --suite-name "timer churn benchmark" \
  --usage-name "benchmark/scripts/run_timer_churn_benchmark.sh" \
  --scenario-fields "sessions,waits" \
  --scenario-format "sessions:waits tuples" \
  --scenarios-default "1:200000,8:80000,32:20000" \
  --iocoro-target "iocoro_timer_churn" \
  --asio-target "asio_timer_churn" \
  --metric-name "ops_s" \
  --ratio-mode "direct" \
  --ratio-field "ratio_vs_asio" \
  --run-timeout-default 120 \
  "$@"
