# dpor

A vibe-coded C++20 DPOR model-checking library inspired by Must.
See the Must paper in docs/.

There are some initial examples of use in the examples/ folder.

## Current model assumptions

- Asynchronous message-passing is the only supported communication model.
- The set of threads is fixed at program construction time — there is no dynamic thread creation during exploration.
- Support blocking and non-blocking receives (the latter can be used to model timeouts as in the 2PC-with-timeouts example) .

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

## Static analysis

The project includes configuration for three integrated static-analysis tools,
all off by default: `clang-tidy`, `cppcheck`, and `IWYU`. The `lint` preset
enables `clang-tidy` and `cppcheck` with enforcing settings
(`WarningsAsErrors: '*'`, `--error-exitcode=1`):

```bash
cmake --preset lint
cmake --build --preset lint
```

The lint preset is self-contained (fetches Catch2 automatically). clang-tidy
requires version 16+ for C++20 structured binding capture support; older
versions are detected at configure time and skipped with a warning.

Individual tools can be enabled on any preset:

```bash
cmake --preset debug -DDPOR_ENABLE_CLANG_TIDY=ON
cmake --preset debug -DDPOR_ENABLE_CPPCHECK=ON
cmake --preset debug -DDPOR_ENABLE_IWYU=ON
```

A clang-format check script is also provided. By default it checks only files
changed relative to `main`:

```bash
scripts/run_clang_format.sh          # check changed files
scripts/run_clang_format.sh --fix    # fix changed files
scripts/run_clang_format.sh --all    # check all project files
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
accelerate hot-path reachability and warm-cache cycle queries:

| Operation | Without cache | With cache |
|---|---|---|
| `porf_contains(from, to)` | O(N+E) per call | O(1) amortized |
| `has_causal_cycle()` | O(N+E) per call | O(1) amortized |

The cache is built on demand via Kahn's topological sort and stores a
per-event vector clock (one entry per thread). Subsequent `porf_contains` calls
compare a single clock entry. Cycle detection is a byproduct of the topological
sort (incomplete sort = cycle). The consistency checker can also use the
cheaper `has_causal_cycle_without_cache()` path when it only needs a cycle
answer and the cache is still cold.

Key properties:

- **Lazy**: vector clocks are only built when `porf_contains()` or the
  cache-backed `has_causal_cycle()` path needs them.
- **Shared across copies**: the cache is held via `std::shared_ptr`, so
  graph copies (from `with_rf`, `with_nd_value`, etc.) share the parent's
  cache until they mutate.
- **Auto-invalidated**: any call to `add_event` or `set_reads_from` resets
  the cache to null, triggering a rebuild on the next query.
- **Cycle-safe for DPOR**: `porf_contains()` throws on cyclic graphs. The DPOR
  engine only calls it on consistent graphs, and consistency checking prunes
  causal cycles before those queries.

## Layout

- `include/dpor/api`: public headers
- `include/dpor/model`: core model types (events, relations, execution/exploration graphs)
- `src`: implementation details
- `tests`: unit/integration tests with CTest
- `examples`: `minimal/` and `two_phase_commit_timeout/`
- `cmake`: packaging and build helper modules
- `docs`: architecture notes
