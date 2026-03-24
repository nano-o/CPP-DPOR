# AGENTS.md

## Project Direction

This project implements a DPOR model-checker inspired by:

- `docs/Enea et al. - 2024 - Model Checking Distributed Protocols in Must.pdf`

The Must paper is our semantic north star for:

- execution graphs (`E`, `po`, `rf`)
- well-formedness and consistency concepts
- async communication semantics and later DPOR exploration strategy

## Important Constraint: Existing Codebases

Unlike Must, we are not designing a new language/API from scratch.
Our target is applying DPOR to existing production codebases (in particular, `stellar-core`), where:

- events are often less structured
- message schemas and handlers already exist
- receive compatibility may only be known by running existing processing logic

## Practical Modeling Rules

To support the above:

- prefer adapter/extraction layers that map runtime behavior into execution-graph events
- avoid hard-coding assumptions that all receives are declaratively enumerable
- support receive matching via predicates/callables, not only finite value sets
- allow matching to defer to system logic (e.g., reject values that produce `INVALID`)

## Determinism Requirements

Any callback/predicate used during exploration (especially receive matching) must be:

- deterministic for the same exploration state
- side-effect free, or run against isolated snapshots

This is required to preserve DPOR soundness/completeness assumptions.

## Current Scope

- prioritize correctness and clarity over optimization
- focus first on async message-passing semantics and the current FIFO point-to-point mode
- add genericity where it improves integration with real systems (not genericity for its own sake)

### Semantics In Scope (Now)

- communication models: async message passing plus the current FIFO point-to-point mode (`FifoP2P`)
- receive semantics: blocking receives plus the current async non-blocking mode, which may observe bottom (`⊥`) when no compatible unread send is taken
- event kinds: send, receive, nondeterministic choice, block, error
- `block` is an internal DPOR event used to represent waiting on a blocking receive; user thread callbacks/adapters must not emit `Block` directly
- thread traces passed to `ThreadFunctionT` contain `ObservedValueT` entries, so non-blocking receives can contribute bottom observations to later control flow
- receive compatibility: predicate/callable-based matching
- exploration strategy: insertion-order execution graphs + backward revisiting

### Semantics Out of Scope (Now)

- broader Must non-blocking semantics beyond the current async/bottom support
- additional communication-model generality beyond the currently implemented `Async` and `FifoP2P` modes
- monitor-specific semantics for temporal properties

## Consistency Invariants Policy

- Every graph that is recursively explored by DPOR (`visit`) must satisfy full consistency (`consistent(G)`), with no issue-code exemptions.
- Intermediate helper constructions (e.g., transient graphs during restrict/remap/revisit computation) may be partial only as internal artifacts.
- No helper/partial graph may be emitted as a complete execution, or used as an exploration state, unless it passes full consistency.

## Build & Test

- **Build system**: CMake 3.22+ with Ninja generator, C++20
- **Test framework**: Catch2 v3
- **CMake presets**: `debug`, `release`, `asan`, `tsan`, `debug-fetch-catch2`, `lint`
- Build: `cmake --preset debug && cmake --build --preset debug`
- Run tests: `ctest --preset debug`
- `ctest -R ...` matches CTest test names, not Catch tags; Catch-discovered tests are prefixed with their executable name, so target-level filters should use names like `ctest --preset debug -R dpor_dpor_test` or `ctest --preset debug -R dpor_two_phase_commit_timeout_test`
- Catch tag expressions such as `[paper]` or `[two_phase_commit]` should be passed to the test binary directly, not to `ctest -R`
- The `debug-fetch-catch2` preset auto-fetches Catch2 if not installed locally
- The `asan` preset enables AddressSanitizer and UndefinedBehaviorSanitizer
- The `tsan` preset enables ThreadSanitizer; use `scripts/run_tsan.sh` to build and run (handles ASLR)
- **After big changes**, run ASAN tests (`cmake --preset asan && cmake --build --preset asan && ctest --preset asan`) and TSAN tests (`scripts/run_tsan.sh`)
- The `lint` preset runs clang-tidy (>= 16) and cppcheck with enforcing settings; use `scripts/run_clang_format.sh` for format checks

## Performance Smoke Tests

`build/bench-release` is a manual benchmark-focused build directory, not a CMake preset.
If it does not exist, configure it as documented in `benchmarks/README.md` before
running the benchmark commands below.

For a lightweight performance smoke test, run DPOR-only with 4 participants,
1 iteration, and no crash injection:

```bash
build/bench-release/benchmarks/two_phase_commit_timeout/dpor_two_phase_commit_timeout_benchmark \
  --mode dpor --participants 4 --iterations 1 --no-crash
```

To sanity-check parallel mode, first run the same workload with `--max-workers 1`:

```bash
build/bench-release/benchmarks/two_phase_commit_timeout/dpor_two_phase_commit_timeout_benchmark \
  --mode dpor --participants 4 --iterations 1 --no-crash \
  --parallel --max-workers 1
```

The single-worker timing should be roughly the same as the non-parallel run.
If it is noticeably slower, the parallel infrastructure is adding unexpected overhead.

## Code Organization

```
include/dpor/
  model/    → dpor::model  (events, relations, graphs, consistency)
  algo/     → dpor::algo   (DPOR engine, program representation)
src/        → internal build helpers only (`header_check.cpp` for warnings/analysis)
tests/      → Catch2 test files
examples/   → two_phase_commit_timeout/
benchmarks/ → standalone benchmark targets and perf helpers
```

- Most code is **header-only templates** in `include/dpor/model/` and `include/dpor/algo/`
- The library is header-only; `src/header_check.cpp` exists only as an internal build target for warnings/analysis

## DPOR Algorithm

The engine in `include/dpor/algo/dpor.hpp` implements **Algorithm 1** from the Must paper:

- `verify()` — sequential DFS entry point, returns `VerifyResult` (`AllExplored` / `Stopped`) plus counts for full, error, and depth-limit terminal executions
- `verify_parallel()` — experimental parallel exploration with configurable worker threads
- `visit()` — recursive exploration of consistent executions
- `backward_revisit()` — identifies alternative interleavings or message matches
- `DporConfigT` — configuration: program, max_depth, communication_model, terminal-execution observer callback (`on_terminal_execution`; legacy alias `on_execution`), and optional progress reporting
- `ParallelVerifyOptions` — parallel tuning: `max_workers`, `max_queued_tasks`, `spawn_depth_cutoff`, `min_fanout`, `sync_steps`, `progress_counter_flush_interval`, `progress_poll_interval_steps`

Programs are defined via `ProgramT` / `ThreadFunctionT` in `include/dpor/algo/program.hpp`.
Thread-function traces use `ObservedValueT` entries rather than raw payloads.

## PorfCache (Vector Clocks)

`ExplorationGraphT` maintains an optional `PorfCache` for O(1) porf (program-order ∪ reads-from)⁺ reachability:

- Built lazily via `ensure_porf_cache()` using vector clocks
- `porf_contains()` requires an acyclic graph; calling it on a cyclic graph throws `std::logic_error`
- This is safe because the DPOR engine only calls `porf_contains` on consistent graphs, and causal cycles are a consistency violation
- Agents modifying `ExplorationGraphT` should be aware that mutations invalidate the cache

## Test Structure

| Test file | Covers |
|---|---|
| `model_types_test.cpp` | Events, labels, ExecutionGraphT |
| `relation_test.cpp` | Relation concept, ExplicitRelation, ProgramOrderRelation, compose, transitive_closure |
| `consistency_test.cpp` | Async and FIFO p2p consistency checking, including all ConsistencyIssueCodes |
| `exploration_graph_test.cpp` | ExplorationGraphT operations (restrict, with_rf, porf_contains, etc.) |
| `dpor_test.cpp` | DPOR algorithm end-to-end (paper examples, FIFO regressions, non-blocking receive coverage, oracle cross-checks, parallel coverage) |
| `dpor_stress_test.cpp` | Randomized stress tests with multiple seeds, including oracle-backed coverage |
| `tests/support/oracle_core.hpp` | Core exhaustive model-aware oracle implementation shared by DPOR correctness tests |
| `tests/support/oracle.hpp` | Catch2-facing model-aware oracle helpers built on top of `oracle_core.hpp` |
| `examples/two_phase_commit_timeout/two_phase_commit_test.cpp` | 2PC protocol + timer behavior example |

## Prototype Policy

This codebase is currently a prototype.

- there is no backward-compatibility commitment
- prioritize clean iteration over preserving old APIs
- when better structure is identified, prefer replacing APIs instead of layering compatibility shims

## Workspace Changes

- Ignore unrelated untracked workspace changes by default.
- Do not remove or rewrite unrelated untracked files unless the user explicitly asks for that.
- Still stop and ask if an unexpected change overlaps the files being modified or creates ambiguity about task correctness.
- When committing, include a message describing what the commit does.
