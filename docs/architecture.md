# Architecture

This document describes the current high-level architecture of the `dpor`
library, a header-only C++20 Dynamic Partial Order Reduction checker for
distributed protocols.

The codebase is organized around four areas:

- **Model**: execution-graph types, relations, and consistency checking
- **Algorithm**: program representation and DPOR exploration
- **Public Entry Points**: sequential and parallel verification APIs
- **Examples**: end-to-end protocol models built on the library

Two cross-cutting pieces support all of them:

- **`include/dpor/errors.hpp`** defines the library-wide exception taxonomy
  (`dpor::error`, `internal_error`, `precondition_error`, `user_code_error`,
  `UserCallbackKind`). Errors in the system under test are modeled as
  `Error` events; C++ exceptions escaping user callbacks are fatal and
  surface as `user_code_error` — except from `on_fatal_error` itself, whose
  exceptions are swallowed so they cannot mask the original failure.
- **`include/dpor/model/format.hpp`** provides `format_graph(...)`, a
  human-readable trace renderer for use in observers and fatal-error
  diagnostics.

## 1. Model Layer (`dpor::model`)

The model layer defines the execution-graph vocabulary and the validity rules
that DPOR explores against.

### Events and Observations

- **`EventT`** is the fundamental unit of execution. Event kinds are `Send`,
  `Receive`, nondeterministic choice, `Block`, and `Error`.
- **`SendLabelT<ValueT>`** keeps the payload and destination thread.
- **`ReceiveLabelT<ValueT>`** carries a blocking/non-blocking mode plus a
  predicate matcher. Receive compatibility is predicate-based rather than
  restricted to finite message sets.
- **`BlockLabel`** is internal to DPOR. User thread callbacks must not emit it.
- **`ObservedValueT<ValueT>`** represents what a thread learns from its local
  history: either a concrete payload or bottom (`⊥`). Bottom arises from
  non-blocking receives that do not consume any compatible unread send.
- **`CommunicationModel`** is a whole-program setting. The currently supported
  models are `Async` and `FifoP2P`.

### Graphs and Reachability

- **`ExecutionGraphT`** stores events plus the reads-from (`rf`) relation. A
  receive reads either from a send or from bottom. Events are append-only and
  both insertion paths enforce strictly increasing per-thread event indices, so
  ascending event id is program order within a thread. `po_relation()` checks
  that invariant rather than sorting to restore it, so a violation surfaces as
  an `internal_error` instead of a silently wrong program order. The append
  helper's own preconditions are verified in `DPOR_VERIFY_GRAPH_INVARIANTS`
  builds, which this build tree enables by default and consumers do not.
- **`ExplorationGraphT`** wraps `ExecutionGraphT` with DPOR-specific state:
  rollback support, per-thread metadata, and cached `(po ∪ rf)+`
  reachability. Event insertion is append-only, so event ids already encode
  insertion order; the diagnostic `insertion_order()` API materializes
  `[0, ..., event_count - 1]` on demand.
- **`PorfCache`** is a lazy vector-clock cache used by `ExplorationGraphT` to
  accelerate hot-path reachability and cycle checks on acyclic graphs. It is
  part of the exploration-graph architecture rather than a detached benchmark
  optimization: DPOR relies on these cached reachability queries in its hot
  path, while graph mutations invalidate the cache and copies can reuse it
  until they diverge. Cache construction uses CSR adjacency, flat row-major
  vector clocks, and reusable per-worker scratch buffers. Kahn's ready set is a
  stack; no consumer depends on which valid topological order is chosen.
- Restrictions rebuild an owned graph through a bulk append path. That path
  reserves storage and reconstructs all derived metadata without populating an
  undo log that the new independent graph would immediately discard.
- The model layer also includes lightweight relation helpers. The most
  important production-facing pieces are **`ProgramOrderRelation`** and
  **`ExplicitRelation`**, which provide views over `po` and `rf`. The generic
  relation algebra (`Relation`, `compose(...)`, `transitive_closure(...)`,
  etc.) is supporting infrastructure rather than a primary architectural axis.

### Consistency

- Consistency checking is model-aware. The public checker types are
  **`AsyncConsistencyCheckerT`**, **`FifoP2PConsistencyCheckerT`**, and
  **`ConsistencyCheckerT(communication_model)`**.
- The checker is structured in layers:
  - graph validation and issue collection for malformed or incomplete `rf`
  - causal-cycle detection over `po ∪ rf`
  - model-specific checks
- `Async` enforces the shared well-formedness and acyclicity rules.
- `FifoP2P` applies the async checks first and then adds the FIFO
  point-to-point constraints from the Must paper's formal definition.

## 2. Algorithm Layer (`dpor::algo`)

The algorithm layer defines the system-under-test interface and the DPOR
exploration engine.

### Program Representation

- **`ProgramT`** is a fixed map of thread callbacks. Threads are declared up
  front; there is no dynamic thread creation during exploration.
- **`ThreadFunctionT`** is a deterministic function of `(trace, step)`.
  `trace` contains only `ObservedValueT<ValueT>` entries produced by receives
  and nondeterministic choices. It does not include send, block, or error
  events, so the separate `step` argument remains the control-flow counter.
- To model runtime thread creation, predeclare the possible threads and keep
  them idle until activated by control flow.

### DPOR Engine

- **`dpor.hpp`** implements the exploration algorithm inspired by Must
  Algorithm 1.
- **`DporConfigT`** carries the program, the DPOR tree-depth limit
  (`max_depth`), the per-thread event bound (`max_thread_events`, `0` =
  unlimited), the whole-program communication model, and optional observers:
  a terminal-execution observer
  (`on_terminal_execution`, with legacy alias `on_execution`; setting both
  throws), a throttled progress observer (`on_progress` plus
  `progress_report_interval`), and a fatal-error observer (`on_fatal_error`).
- **`verify()`** performs sequential exploration.
- **`verify_parallel()`** is an experimental parallel executor built on the
  same DPOR core and configuration.
- The current exploration core is iterative: one mutable graph is explored
  through explicit continuation frames, with owned child contexts only for
  branches that cannot be reached by rollback from the current graph.
- Exploration proceeds over consistent execution graphs and includes:
  - forward branching on enabled events
  - Must-style internal `Block` insertion for unsatisfied blocking receives
  - rescheduling of blocked receives before treating an execution as terminal
  - non-blocking receive branching over compatible sends plus the bottom branch
  - backward revisiting to recover alternative message matches and
    interleavings
- Revisit and tiebreaker logic are communication-model aware. The masked
  `G|Previous` tiebreaker evaluates reachability and the formal FIFO clauses
  directly against the keep mask, without materializing the restriction. The
  former restriction-based implementation remains available as a differential
  oracle in `DPOR_VERIFY_MASKED_TIEBREAKER` builds.

### Results and Observers

- Verification reports `AllExplored` or `Stopped`.
- Optional terminal-execution observers receive `TerminalExecutionT<ValueT>`
  values for each full execution, blocked maximal execution, error execution,
  depth-limit execution, and thread-event-limit execution. The last two mark
  branches the engine may have truncated rather than maximal executions:
  `DepthLimit` when the branch hit `max_depth`, `ThreadEventLimit` when the
  engine declined to ask some nonterminated thread for a further event because
  it sat at `max_thread_events`.
- Observers may request early termination by returning `Stop`. In parallel
  mode stop is best-effort: workers batch stop checks (`sync_steps`), so
  additional terminal executions may be published after stop is requested.
- Optional progress observers receive throttled `ProgressSnapshot` values
  (state, elapsed time, live terminal counts) while exploration runs. In
  parallel mode, counter freshness and polling are tuned via
  `progress_counter_flush_interval` and `progress_poll_interval_steps`.
- An optional fatal-error observer receives a `FatalErrorContextT` diagnostic
  (including the in-progress graph) before a fatal exception raised during
  active exploration propagates out of `verify()`/`verify_parallel()`.
- `VerifyResult` tracks total published terminal executions plus a split count
  for each terminal-execution kind, and `max_thread_event_depth_reached`, the
  largest per-thread event count over the published terminals.

## 3. Public Entry Points

The main public exploration APIs are:

- `dpor::algo::verify()`
- `dpor::algo::verify_parallel()`

The most important supporting public types are:

- `dpor::algo::ProgramT<ValueT>`
- `dpor::algo::DporConfigT<ValueT>`
- `dpor::algo::ParallelVerifyOptions`
- `dpor::model::ExplorationGraphT<ValueT>`
- `dpor::model::ConsistencyCheckerT<ValueT>`

## 4. Examples (`examples/`)

The repository includes end-to-end examples that exercise the intended
integration style:

- **`two_phase_commit_timeout/`** models a Two-Phase Commit protocol with
  timers, timeout-driven control flow, and a UDP-backed environment adapter.

---

## Design Principles

- **Correctness before optimization**: the current implementation prefers
  explicit, reviewable semantics over aggressive reduction shortcuts.
- **Determinism and isolation**: for a fixed program and configuration,
  sequential exploration (`verify()`) is deterministic. Thread functions and
  receive matchers must be deterministic and must not leak state between
  invocations; adapters may mutate isolated snapshots. Parallel exploration
  covers the same execution set when callbacks are concurrency-safe, but
  callback order across workers is unspecified, and runs that use `Stop` may
  differ in how many terminals are published before stopping.
- **Separation of concerns**: execution validity is defined by the model layer,
  while search strategy lives in the DPOR engine.
- **Isolated ownership at task boundaries**: parallel tasks own graph values,
  while worker-local exploration mutates graphs temporarily and restores them
  via rollback.
- **No hidden shared state across runs**: exploration state lives in configs,
  executors, results, and graph values. The one deliberate exception is the
  parallel executor's per-OS-thread `thread_local` worker state, which is
  scoped by a reentrancy precondition check rather than shared globally.
