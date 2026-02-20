# Architecture Notes

This scaffold starts with a minimal API and is organized to scale toward a full DPOR checker.

## Suggested module growth

- `include/dpor/model`: events, traces, dependency relation, happens-before graph
- `include/dpor/algo`: source-DPOR, persistent sets, sleep sets, reduction strategy interfaces
- `include/dpor/runtime`: scheduler/executor abstractions for system adapters
- `include/dpor/api`: top-level entry points and stable integration surface

## Design constraints

- Keep public API headers lean and explicit.
- Avoid global state in exploration logic.
- Make exploration deterministic when seeded.
- Separate model extraction from reduction/exploration.
