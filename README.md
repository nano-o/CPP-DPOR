# dpor

A vibe-coded C++20 DPOR model-checking library inspired by Must.
See the Must paper in docs/.


## Goals

- Keep a small stable API for consumers.
- Make algorithm internals easy to evolve.
- Ship as a reusable CMake package (`dpor::dpor`).

## Current model assumptions

- The set of threads is fixed at program construction time — there is no
  dynamic thread creation during exploration.
- Asynchronous message-passing is the only supported communication model.
- Blocking receives are supported.
- Non-blocking receives are also supported in the current async model: they
  may consume a compatible unread send or observe bottom (`⊥`) when no send is
  taken.
- Thread traces expose receive outcomes as `ObservedValueT<ValueT>`, so
  thread functions can distinguish a payload from bottom.
- `Block` events are internal to the DPOR engine (used for temporarily
  unsatisfied blocking receives), not user-program events.

## Build

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Tests are written with Catch2 (v3). CMake uses a system Catch2 package if available.
Paper-derived examples in current scope are in `tests/dpor_test.cpp` and tagged `[paper]`.
`tests/dpor_test.cpp` and `tests/dpor_stress_test.cpp` also cross-check DPOR
against an independent exhaustive async oracle in `tests/support/oracle.hpp`.
When `max_depth` truncates exploration, `verify()` reports
`DepthLimitReached`.

If Catch2 is not installed and your environment has internet access, enable fetching:

```bash
cmake --preset debug-fetch-catch2
cmake --build --preset debug-fetch-catch2
ctest --preset debug-fetch-catch2
```

## Editor setup (clangd)

All presets export `compile_commands.json`. Symlink it to the project root so clangd can find it:

```bash
ln -sf build/debug/compile_commands.json .
```

## Install

```bash
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix /tmp/dpor-install
```

## Consume from another CMake project

```cmake
find_package(dpor CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE dpor::dpor)
```

If installed to a non-default prefix, add it to `CMAKE_PREFIX_PATH`.

## Performance: lazy PorfCache with vector clocks

`ExplorationGraphT` uses a lazy, shared vector-clock cache (`PorfCache`) to
accelerate two hot-path queries:

| Operation | Without cache | With cache |
|---|---|---|
| `porf_contains(from, to)` | O(N+E) per call | O(1) amortized |
| `has_causal_cycle()` | O(N+E) per call | O(1) amortized |

The cache is built on first query via Kahn's topological sort and stores a
per-event vector clock (one entry per thread). Subsequent `porf_contains` calls
compare a single clock entry. Cycle detection is a byproduct of the topological
sort (incomplete sort = cycle).

Key properties:

- **Lazy**: the cache is only built when `porf_contains` or `has_causal_cycle`
  is first called.
- **Shared across copies**: the cache is held via `std::shared_ptr`, so
  graph copies (from `with_rf`, `with_nd_value`, etc.) share the parent's
  cache until they mutate.
- **Auto-invalidated**: any call to `add_event` or `set_reads_from` resets
  the cache to null, triggering a rebuild on the next query.
- **Cycle-safe**: if the graph contains a causal cycle, `porf_contains` falls
  back to the original BFS/transitive-closure implementation.

## Layout

- `include/dpor/api`: public, stable headers
- `include/dpor/model`: core model types (events, relations, execution/exploration graphs)
- `src`: implementation details
- `tests`: unit/integration tests with CTest
- `examples`: `minimal/`, `two_phase_commit/`, and `two_phase_commit_timeout/`
- `cmake`: packaging and build helper modules
- `docs`: architecture notes
