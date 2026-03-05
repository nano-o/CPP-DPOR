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
- focus first on async communication model support
- add genericity where it improves integration with real systems (not genericity for its own sake)

### Semantics In Scope (Now)

- communication model: async message passing only
- receive semantics: blocking receives only
- event kinds: send, receive, nondeterministic choice, block, error
- `block` is an internal DPOR event used to represent waiting on a blocking receive; user thread callbacks/adapters must not emit `Block` directly
- receive compatibility: predicate/callable-based matching
- exploration strategy: insertion-order execution graphs + backward revisiting

### Semantics Out of Scope (Now)

- non-blocking receives / bottom (`⊥`) receive results
- multiple communication models (`p2p`, `cd`, `mbox`) and their model-specific consistency rules
- monitor-specific semantics for temporal properties

## Consistency Invariants Policy

- Every graph that is recursively explored by DPOR (`visit`) must satisfy full consistency (`consistent(G)`), with no issue-code exemptions.
- Intermediate helper constructions (e.g., transient graphs during restrict/remap/revisit computation) may be partial only as internal artifacts.
- No helper/partial graph may be emitted as a complete execution, or used as an exploration state, unless it passes full consistency.

## Build & Test

- **Build system**: CMake 3.22+ with Ninja generator, C++20
- **Test framework**: Catch2 v3
- **CMake presets**: `debug`, `release`, `asan`, `debug-fetch-catch2`
- Build: `cmake --preset debug && cmake --build --preset debug`
- Run tests: `ctest --preset debug`
- The `debug-fetch-catch2` preset auto-fetches Catch2 if not installed locally
- The `asan` preset enables AddressSanitizer and UndefinedBehaviorSanitizer

## Code Organization

```
include/dpor/
  model/    → dpor::model  (events, relations, graphs, consistency)
  algo/     → dpor::algo   (DPOR engine, program representation)
  api/      → dpor::api    (Session — public entry point)
src/api/    → only compiled source (session.cpp)
tests/      → Catch2 test files
examples/   → minimal/ and two_phase_commit/
```

- Most code is **header-only templates** in `include/dpor/model/` and `include/dpor/algo/`
- The only `.cpp` source file is `src/api/session.cpp`; don't look for `.cpp` files for model/algo code

## DPOR Algorithm

The engine in `include/dpor/algo/dpor.hpp` implements **Algorithm 1** from the Must paper:

- `verify()` — top-level entry point, returns `VerifyResult` (AllExecutionsExplored / ErrorFound / DepthLimitReached)
- `visit()` — recursive exploration of consistent executions
- `backward_revisit()` — identifies alternative interleavings or message matches
- `DporConfigT` — configuration: program, max_depth, on_execution observer callback

Programs are defined via `ProgramT` / `ThreadFunctionT` in `include/dpor/algo/program.hpp`.

## PorfCache (Vector Clocks)

`ExplorationGraphT` maintains an optional `PorfCache` for O(1) porf (program-order ∪ reads-from)⁺ reachability:

- Built lazily via `ensure_porf_cache()` using vector clocks
- `porf_contains()` requires an acyclic graph; calling it on a cyclic graph throws `std::logic_error`
- This is safe because the DPOR engine only calls `porf_contains` on consistent graphs, and causal cycles are a consistency violation
- Agents modifying `ExplorationGraphT` should be aware that mutations invalidate the cache

## Test Structure

| Test file | Covers |
|---|---|
| `session_test.cpp` | Session/SessionConfig API |
| `model_types_test.cpp` | Events, labels, ExecutionGraphT |
| `relation_test.cpp` | Relation concept, ExplicitRelation, ProgramOrderRelation, compose, transitive_closure |
| `consistency_test.cpp` | AsyncConsistencyCheckerT, all ConsistencyIssueCodes |
| `exploration_graph_test.cpp` | ExplorationGraphT operations (restrict, with_rf, porf_contains, etc.) |
| `dpor_test.cpp` | DPOR algorithm end-to-end (paper examples, known execution counts) |
| `dpor_stress_test.cpp` | Randomized stress tests with multiple seeds |
| `two_phase_commit_test.cpp` | 2PC protocol example (in `examples/two_phase_commit/`) |

## Prototype Policy

This codebase is currently a prototype.

- there is no backward-compatibility commitment
- prioritize clean iteration over preserving old APIs
- when better structure is identified, prefer replacing APIs instead of layering compatibility shims
