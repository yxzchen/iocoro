# Architecture Boundaries

This file documents layering rules intended to keep public APIs stable and avoid leaking
implementation details.

## Layers

- `include/iocoro/*.hpp`:
  Public surface and user contracts.
- `include/iocoro/detail/*.hpp`:
  Internal contracts shared by public adapters and implementations.
- `include/iocoro/impl/*.ipp`:
  Implementation details.

## Rules

- Public headers must not require consumers to name `detail::*` types.
- Public semantics must be described at the API boundary (`*.hpp`), not only in implementation
  comments.
- `detail` and `impl` may change freely between releases; public headers should minimize coupling
  to these internals.
- Tests may include internals for verification, but production usage examples should prefer public
  APIs.

## Executor / I/O Contract Notes

- `io_context::run_one()` is a single scheduler turn and can complete multiple callbacks.
- `thread_pool` is best-effort after `stop()` and may drop new `post()` calls.
- `with_timeout()` is cooperative cancel+join, not preemptive cancellation.
