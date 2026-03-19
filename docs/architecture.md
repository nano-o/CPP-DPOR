# Architecture

This document describes the current high-level architecture of the `dpor`
library, a header-only C++20 Dynamic Partial Order Reduction checker for
distributed protocols.

The codebase is organized around four areas:

- **Model**: execution-graph types, relations, and consistency checking
- **Algorithm**: program representation and DPOR exploration
- **Public Entry Points**: sequential and parallel verification APIs
- **Examples**: end-to-end protocol models built on the library

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
  receive reads either from a send or from bottom.
- **`ExplorationGraphT`** wraps `ExecutionGraphT` with DPOR-specific state:
  insertion order, rollback support, thread-local metadata, and cached
  `(po ∪ rf)+` reachability.
- **`PorfCache`** is a lazy vector-clock cache used by `ExplorationGraphT` to
  accelerate hot-path reachability and cycle checks on acyclic graphs. It is
  part of the exploration-graph architecture rather than a detached benchmark
  optimization: DPOR relies on these cached reachability queries in its hot
  path, while graph mutations invalidate the cache and copies can reuse it
  until they diverge.
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
- **`DporConfigT`** carries the program, depth limit, whole-program
  communication model, and an optional terminal-execution observer.
- **`verify()`** performs sequential exploration.
- **`verify_parallel()`** is an experimental parallel executor built on the
  same DPOR core and configuration.
- Exploration proceeds over consistent execution graphs and includes:
  - forward branching on enabled events
  - Must-style internal `Block` insertion for unsatisfied blocking receives
  - rescheduling of blocked receives before treating an execution as terminal
  - non-blocking receive branching over compatible sends plus the bottom branch
  - backward revisiting to recover alternative message matches and
    interleavings
- Revisit and tiebreaker logic are communication-model aware. In particular,
  FIFO point-to-point runs use conservative consistency checks rather than the
  async-only masked shortcut.

### Results and Observers

- Verification reports `AllExecutionsExplored`, `ErrorFound`, or
  `DepthLimitReached`.
- Optional terminal-execution observers receive `TerminalExecutionT<ValueT>`
  values for each full execution, error execution, and depth-limit execution.
- `VerifyResult` tracks total published terminal executions plus a split count
  for each terminal-execution kind.

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
- **Determinism**: for a fixed program and configuration, exploration is
  deterministic. Receive matchers and other callbacks must therefore be
  deterministic and side-effect free.
- **Separation of concerns**: execution validity is defined by the model layer,
  while search strategy lives in the DPOR engine.
- **Isolated ownership at task boundaries**: parallel tasks own graph values,
  while worker-local recursion mutates graphs temporarily and restores them via
  rollback.
- **Zero global state**: exploration state lives in configs, executors,
  results, and graph values rather than hidden global mutable state.
