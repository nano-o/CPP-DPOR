# dpor

A vibe-coded C++20 DPOR model-checking library inspired by Must.
See the Must paper in docs/.

There are some initial examples of use in the examples/ folder.
See `docs/api.md` for a public API summary and `docs/architecture.md` for the
high-level design.

## Current scope

- Supported communication models are `Async` and `FifoP2P`.
- The set of threads is fixed at program construction time — there is no dynamic thread creation during exploration.
- Supports blocking and non-blocking receives. Non-blocking receives may observe
  bottom (`⊥`), which is useful for timeout-style modeling such as the 2PC
  example.
- Event kinds are send, receive, nondeterministic choice, block, and error.
  `block` is an internal DPOR event; user thread functions should not emit it.
- Receive compatibility is predicate-based, with helpers for both arbitrary
  matchers and finite accepted-value sets.

## Public API

- Main exploration entry points: `dpor::algo::verify()` and experimental
  `dpor::algo::verify_parallel()`.
- Programs are modeled as `dpor::algo::ProgramT<ValueT>` collections of
  deterministic thread functions.
- Published terminal executions are exposed to observers as
  `dpor::algo::TerminalExecutionT<ValueT>`, which carries an
  `ExplorationGraphT<ValueT>` plus a terminal kind (`Full`, `Error`, or
  `DepthLimit`).
- Manual graph validation is available through
  `dpor::model::AsyncConsistencyCheckerT`, `FifoP2PConsistencyCheckerT`, and
  `ConsistencyCheckerT`.
- The user-facing API is summarized in `docs/api.md`.

## Build

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

CTest vs Catch filtering:

- `ctest -R ...` matches CTest test names, not Catch tags.
- Catch-discovered tests are prefixed with their executable name, so
  `ctest --preset debug -R dpor_dpor_test` runs the tests discovered from
  `dpor_dpor_test`, and
  `ctest --preset debug -R dpor_two_phase_commit_timeout_test` runs the 2PC
  example tests.
- Catch tag expressions such as `[paper]` or `[two_phase_commit]` only work on
  the test binaries themselves, for example
  `./build/debug/examples/two_phase_commit_timeout/dpor_two_phase_commit_timeout_test "[two_phase_commit]"`.

Tests are written with Catch2 (v3). CMake uses a system Catch2 package if available.
Paper-derived examples in current scope are in `tests/dpor_test.cpp` and tagged `[paper]`.
`tests/dpor_test.cpp` and `tests/dpor_stress_test.cpp` also cross-check DPOR
against an independent exhaustive async oracle in `tests/support/oracle.hpp`.
When `max_depth` truncates exploration, DPOR publishes depth-limit terminal
executions and counts them in `VerifyResult::depth_limit_executions_explored`.
The current exploration code still uses recursive branch traversal, so very
deep executions may also hit the process stack before reaching the configured
depth limit.

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
For the header-only library itself, these checks run through the internal
`dpor_header_check` target, which compiles a translation unit that includes the
public headers.

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

| Operation                 | Without cache   | With cache     |
|---------------------------|-----------------|----------------|
| `porf_contains(from, to)` | O(N+E) per call | O(1) amortized |
| `has_causal_cycle()`      | O(N+E) per call | O(1) amortized |

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

- `include/dpor/model`: core model types (events, relations, execution/exploration graphs)
- `include/dpor/algo`: DPOR engine and program representation
- `src`: internal build helpers only (for example, the header-check translation unit)
- `tests`: unit/integration tests with CTest
- `examples`: `two_phase_commit_timeout/`
- `cmake`: packaging and build helper modules
- `docs`: API and architecture notes
