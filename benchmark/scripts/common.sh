#!/usr/bin/env bash

bench_require_non_negative_int() {
  local name="$1"
  local value="$2"
  if ! [[ "$value" =~ ^[0-9]+$ ]]; then
    echo "$name must be a non-negative integer" >&2
    return 1
  fi
}

bench_require_positive_int() {
  local name="$1"
  local value="$2"
  bench_require_non_negative_int "$name" "$value" || return 1
  if [[ "$value" -le 0 ]]; then
    echo "$name must be > 0" >&2
    return 1
  fi
}

bench_to_abs_path() {
  local project_dir="$1"
  local path="$2"
  if [[ "$path" == /* ]]; then
    printf '%s\n' "$path"
  else
    printf '%s\n' "$project_dir/$path"
  fi
}

bench_run_cmd_with_timeout() {
  local timeout_sec="$1"
  shift

  local out
  local code=0
  if [[ "$timeout_sec" -gt 0 ]] && command -v timeout >/dev/null 2>&1; then
    set +e
    out="$(timeout --foreground "${timeout_sec}s" "$@" 2>&1)"
    code=$?
    set -e
    if [[ $code -ne 0 ]]; then
      if [[ $code -eq 124 ]]; then
        echo "Benchmark timed out after ${timeout_sec}s: $*" >&2
      else
        echo "Benchmark command failed ($code): $*" >&2
        echo "$out" >&2
      fi
      return 1
    fi
  else
    set +e
    out="$("$@" 2>&1)"
    code=$?
    set -e
    if [[ $code -ne 0 ]]; then
      echo "Benchmark command failed ($code): $*" >&2
      echo "$out" >&2
      return 1
    fi
  fi
  printf '%s\n' "$out"
}

bench_extract_metric() {
  local metric="$1"
  local line="$2"
  sed -nE "s/.* ${metric}=([0-9]+([.][0-9]+)?).*/\\1/p" <<<"$line"
}

bench_median_from_stdin() {
  awk '
    { vals[++n] = $1; }
    END {
      if (n == 0) { print "0"; exit; }
      mid = int((n + 1) / 2);
      if (n % 2 == 1) {
        printf "%.2f\n", vals[mid];
      } else {
        printf "%.2f\n", (vals[mid] + vals[mid + 1]) / 2.0;
      }
    }
  '
}

bench_compute_ratio() {
  local lhs="$1"
  local rhs="$2"
  awk -v i="$lhs" -v a="$rhs" 'BEGIN { if (a <= 0) { print "0.00"; } else { printf "%.4f\n", i / a; } }'
}
