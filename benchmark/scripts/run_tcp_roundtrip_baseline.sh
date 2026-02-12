#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

exec "$SCRIPT_DIR/run_ratio_baseline_suite.sh" \
  --suite-name "tcp roundtrip benchmark" \
  --usage-name "benchmark/scripts/run_tcp_roundtrip_baseline.sh" \
  --scenario-fields "sessions,msgs,msg_bytes" \
  --scenario-format "sessions:msgs[:msg_bytes] tuples" \
  --scenario-min-arity 2 \
  --scenario-default-values ",,13" \
  --scenarios-default "1:5000:64,8:2000:64,32:500:64,8:1000:1024,32:200:4096" \
  --baseline-description "Threshold file: legacy=\"sessions msgs min_ratio\" (msg_bytes=13), new=\"sessions msgs msg_bytes min_ratio\"" \
  --baseline-legacy-arity 2 \
  --baseline-legacy-match-index 3 \
  --baseline-legacy-match-value 13 \
  --iocoro-target "iocoro_tcp_roundtrip" \
  --asio-target "asio_tcp_roundtrip" \
  --metric-names "rps" \
  --primary-metric "rps" \
  --ratio-mode "direct" \
  --ratio-field "ratio_vs_asio" \
  --ratio-label "iocoro/asio" \
  --run-timeout-default 60 \
  "$@"
