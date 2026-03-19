# API

This document summarizes the public API exposed by the header-only `dpor`
library. The implementation lives under `include/dpor/` and is split into two
namespaces:

- `dpor::algo`: program representation and DPOR entry points
- `dpor::model`: events, graphs, consistency checkers, and relation helpers

The library is templated on `ValueT` so integrations can use native payload
types. Default aliases such as `dpor::algo::Program` and
`dpor::model::EventLabel` use `std::string` payloads.

## Headers

- `include/dpor/algo/program.hpp`
- `include/dpor/algo/dpor.hpp`
- `include/dpor/model/event.hpp`
- `include/dpor/model/execution_graph.hpp`
- `include/dpor/model/exploration_graph.hpp`
- `include/dpor/model/consistency.hpp`
- `include/dpor/model/relation.hpp`

## Quick start

```cpp
#include "dpor/algo/dpor.hpp"

using dpor::algo::DporConfig;
using dpor::algo::Program;
using dpor::model::EventLabel;
using dpor::model::SendLabel;
using dpor::model::Value;
using dpor::model::make_receive_label;

Program program;

program.threads[1] = [](const dpor::algo::ThreadTrace&,
                        std::size_t step) -> std::optional<EventLabel> {
  if (step == 0) {
    return SendLabel{.destination = 2, .value = "x"};
  }
  return std::nullopt;
};

program.threads[2] = [](const dpor::algo::ThreadTrace& trace,
                        std::size_t) -> std::optional<EventLabel> {
  if (trace.empty()) {
    return make_receive_label<Value>();
  }
  return std::nullopt;
};

DporConfig config;
config.program = std::move(program);

const auto result = dpor::algo::verify(config);
```

## Programs and thread functions

`ProgramT<ValueT>` contains a fixed set of thread callbacks:

```cpp
template <typename ValueT>
using ThreadFunctionT = std::function<std::optional<model::EventLabelT<ValueT>>(
    const ThreadTraceT<ValueT>&, std::size_t step)>;

template <typename ValueT>
struct ProgramT {
  ThreadMapT<ThreadFunctionT<ValueT>> threads;
};
```

Important rules:

- Thread IDs must form a compact contiguous 0-based or 1-based range.
- Thread functions must be deterministic and side-effect free for the same
  `(trace, step)` inputs.
- `trace` contains only values observed through receives and nondeterministic
  choices. It does not include send, block, or error events.
- Use `step`, not `trace.size()`, as the control-flow counter.
- User thread functions must not emit `BlockLabel`; DPOR inserts block events
  internally when a blocking receive has no compatible unread send.

`ObservedValueT<ValueT>` represents either a concrete payload or bottom (`⊥`).
This matters for non-blocking receives, which may observe bottom when they do
not consume any unread compatible send.

## Event model

The public event vocabulary is:

- `SendLabelT<ValueT>{ ThreadId destination, ValueT value }`
- `ReceiveLabelT<ValueT>{ ReceiveMode mode, ReceiveMatchFnT<ValueT> matches }`
- `NondeterministicChoiceLabelT<ValueT>{ ValueT value, std::vector<ValueT> choices }`
- `ErrorLabel{ std::string message }`
- `BlockLabel` for internal DPOR use

Supporting enums and helpers:

- `CommunicationModel::{ Async, FifoP2P }`
- `ReceiveMode::{ Blocking, NonBlocking }`
- `match_any_value<ValueT>()`
- `make_receive_label<ValueT>(matcher, mode)`
- `make_nonblocking_receive_label<ValueT>(matcher)`
- `make_receive_label_from_values<ValueT>(accepted_values, mode)`

Receive matching is predicate-based. Matchers must be deterministic and
side-effect free.

## Running exploration

The main configuration type is:

```cpp
template <typename ValueT>
struct DporConfigT {
  ProgramT<ValueT> program;
  std::size_t max_depth{1000};
  model::CommunicationModel communication_model{model::CommunicationModel::Async};
  ExecutionObserverT<ValueT> on_execution{};
};
```

Public entry points:

- `VerifyResult verify(const DporConfigT<ValueT>& config)`
- `VerifyResult verify_parallel(const DporConfigT<ValueT>& config,
  ParallelVerifyOptions options = {})`

`verify_parallel()` is experimental. Its tuning options are:

```cpp
struct ParallelVerifyOptions {
  std::size_t max_workers{0};
  std::size_t max_queued_tasks{0};
  std::size_t spawn_depth_cutoff{0};
  std::size_t min_fanout{2};
  std::size_t sync_steps{0};
};
```

`VerifyResult` reports:

- `VerifyResultKind::AllExecutionsExplored`
- `VerifyResultKind::ErrorFound`
- `VerifyResultKind::DepthLimitReached`

and also carries:

- `message`: populated for error terminals
- `executions_explored`: number of published complete or error executions

If `on_execution` is set, DPOR calls it with each published
`ExplorationGraphT<ValueT>`.
Published executions are complete/quiescent executions and error terminals.
Branches truncated by `max_depth` are not published and do not trigger
`on_execution`, so the callback does not receive a per-execution depth-limit
reason.

## Execution graphs

### `ExecutionGraphT<ValueT>`

`ExecutionGraphT` is the low-level graph representation for a single execution.
Useful APIs include:

- `add_event(thread, label)`
- `add_event_with_index(thread, index, label)`
- `set_reads_from(receive_id, send_id)`
- `set_reads_from_bottom(receive_id)`
- `event(id)`, `events()`, `is_valid_event_id(id)`
- `reads_from()`
- `po_relation()`
- `rf_relation()`
- `receive_event_ids()`, `send_event_ids()`, `unread_send_event_ids()`

Reads-from entries map a receive either to a send or to bottom.

### `ExplorationGraphT<ValueT>`

`ExplorationGraphT` wraps `ExecutionGraphT` with DPOR-specific state such as
insertion order, rollback support, and cached `(po ∪ rf)+` reachability.

Inspection APIs:

- `event(id)`, `events()`, `event_count()`
- `reads_from()`
- `insertion_order()`
- `inserted_before_or_equal(a, b)`
- `po_relation()`, `rf_relation()`
- `receive_event_ids()`, `send_event_ids()`, `unread_send_event_ids()`
- `thread_trace(tid)`, `thread_event_count(tid)`, `last_event_id(tid)`
- `thread_is_terminated(tid)`
- `porf_contains(from, to)`
- `has_porf_cache()`
- `is_known_acyclic()`
- `has_causal_cycle()`
- `execution_graph()`

Graph transformation and rollback helpers:

- `restrict(keep_set)`
- `with_rf(recv, send)`
- `with_rf_preserving_known_acyclicity(recv, send)`
- `with_rf_source(recv, source)`
- `with_bottom_rf(recv)`
- `rebind_rf_preserving_known_acyclicity(recv, send)`
- `with_nd_value(nd_event, value)`
- `checkpoint()`
- `rollback(checkpoint)`
- `ScopedRollback`

`porf_contains()` requires an acyclic graph and throws on causal cycles.

## Consistency checking

The public checker types are:

- `AsyncConsistencyCheckerT<ValueT>`
- `FifoP2PConsistencyCheckerT<ValueT>`
- `ConsistencyCheckerT<ValueT>(communication_model)`

Each accepts either an `ExecutionGraphT<ValueT>` or an
`ExplorationGraphT<ValueT>` and returns:

```cpp
struct ConsistencyResult {
  std::vector<ConsistencyIssue> issues;

  bool is_consistent() const noexcept;
};
```

Issue codes currently cover:

- invalid event references
- malformed reads-from endpoints
- missing reads-from assignments for receives
- blocking receives reading bottom
- multiple receives consuming the same send
- destination or value mismatches
- causal cycles
- FIFO point-to-point ordering violations

`CommunicationModel::FifoP2P` applies the async checks first and then adds the
extra FIFO rules.

## Relation helpers

The relation layer is intentionally generic. Public pieces are:

- `Relation` concept
- `ExplicitRelation`
- `ProgramOrderRelation`
- `compose(left, right)`
- `transitive_closure(relation)`
- `relation_union(left, right)`

These are useful when downstream code wants to inspect or derive relations from
`po` and `rf` directly.

## Practical notes

- Prefer `ExplorationGraphT` in `on_execution` observers; it exposes the most
  useful query surface for inspecting explored executions.
- Prefer `ExecutionGraphT` when manually constructing a graph for tests or
  checking consistency outside the DPOR engine.
- Predicate-based receives are part of the intended integration model for
  existing systems. They are not limited to finite value sets.
- Soundness depends on determinism. Mutable captures, time-dependent matchers,
  and other side effects can invalidate exploration guarantees.
