#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

exec "$SCRIPT_DIR/../run_perf_ratio_suite.sh" \
  --suite-name "thread_pool_scaling benchmark suite" \
  --usage-name "benchmark/scripts/suites/run_perf_thread_pool_scaling.sh" \
  --scenario-fields "workers,tasks" \
  --scenario-format "workers:tasks tuples" \
  --scenarios-default "1:200000,2:400000" \
  --iocoro-target "iocoro_thread_pool_scaling" \
  --asio-target "asio_thread_pool_scaling" \
  --metric-name "ops_s" \
  --ratio-mode "direct" \
  --ratio-field "ratio_vs_asio" \
  --run-timeout-default 120 \
  "$@"
