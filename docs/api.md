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

- `include/dpor/errors.hpp`
- `include/dpor/algo/program.hpp`
- `include/dpor/algo/dpor.hpp`
- `include/dpor/model/event.hpp`
- `include/dpor/model/execution_graph.hpp`
- `include/dpor/model/exploration_graph.hpp`
- `include/dpor/model/consistency.hpp`
- `include/dpor/model/format.hpp`
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
- Thread functions must be deterministic for the same `(trace, step)` inputs
  and must not leak side effects between invocations. Stateful adapters should
  replay against fresh or otherwise isolated snapshots.
- Parallel verification may invoke thread functions concurrently, so captured
  state must also be safe for concurrent use.
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
must not leak side effects between invocations; matcher logic that calls into
the system under test must use isolated snapshots. Parallel verification may
evaluate matchers concurrently.

When a nondeterministic-choice label has a non-empty `choices` vector, DPOR
treats it as a set: duplicate values are removed while preserving first
occurrence order. In that case `ValueT` must support equality and ordering
(`operator==` and `operator<`). An empty `choices` vector records the supplied
`value` as a linear event without branching.

## Running exploration

The main configuration type is:

```cpp
template <typename ValueT>
struct DporConfigT {
  ProgramT<ValueT> program;
  std::size_t max_depth{1000};
  std::size_t max_thread_events{0};
  model::CommunicationModel communication_model{model::CommunicationModel::Async};
  TerminalExecutionObserverT<ValueT> on_terminal_execution{};
  ProgressObserver on_progress{};
  std::chrono::milliseconds progress_report_interval{std::chrono::seconds(1)};
  TerminalExecutionObserverT<ValueT> on_execution{};  // Legacy alias.
  FatalErrorObserverT<ValueT> on_fatal_error{};
};
```

`max_depth` bounds logical DPOR tree depth, not current graph size or
implementation stack depth.

`max_thread_events` bounds the events any single thread may contribute; `0`
means unlimited. It is a per-thread event-graph bound rather than a search-tree
bound: each thread is capped independently, and reaching the cap on one thread
does not truncate the others. In what gets explored it is exactly equivalent to
wrapping every thread function as
`f'(trace, step) = step >= max_thread_events ? nullopt : f(trace, step)`; the
engine skips the call instead of paying for it. The difference is the terminal
classification described below.

Set only one of `on_terminal_execution` and its legacy alias `on_execution`;
setting both is a `dpor::precondition_error`.

The observer receives:

```cpp
enum class TerminalExecutionKind : std::uint8_t {
  Full,
  Blocked,
  Error,
  DepthLimit,
  ThreadEventLimit,
};

template <typename ValueT>
struct TerminalExecutionT {
  const model::ExplorationGraphT<ValueT>& graph;
  TerminalExecutionKind kind;
};

enum class TerminalExecutionAction : std::uint8_t { Continue, Stop };
```

`Full` and `Blocked` partition the maximal executions: `Full` means every
thread ran to completion, while `Blocked` means at least one thread ended
waiting forever on a blocking receive that no message can satisfy. The graph
of a blocked execution contains the internal `Block` events marking which
threads are stuck (always as the last event of their thread). `DepthLimit`
branches keep that kind even if some thread happens to be blocked at the
cutoff, because a truncated branch is not a maximal execution.

`ThreadEventLimit` says the engine declined to ask at least one nonterminated
thread whether it had a further event, because that thread sat at
`max_thread_events`. Like `DepthLimit` it is not a maximal execution and keeps
that kind even if some other thread happens to be blocked at the cutoff. It is
deliberately conservative: a thread whose function would have returned
`nullopt` at exactly `step == max_thread_events` cannot be told apart from a
genuinely truncated one without making the call the bound exists to avoid, so
the kind means "this execution may be truncated", not "this execution was
truncated". A thread whose `max_thread_events`-th event is an engine-injected
`Block` is *not* in this category: it has terminated, so the engine never
declines to ask it, the execution classifies as `Blocked`, and the `Block`
stays eligible for blocked-receive rescheduling (which re-asks at
`step == max_thread_events - 1`, still under the bound).

Terminal-kind precedence is
`DepthLimit > Error > ThreadEventLimit > Blocked > Full`. That order follows
from the control flow -- `max_depth` is checked before `next_P(G)` is computed
and an `Error` label is published as soon as it is returned, so the
cap-suppression signal is only ever consulted on the no-next-event path.

A blocked execution is not necessarily a bug. In request/response protocols a
blocked terminal usually is one (a node waiting for a message that can never
arrive), and deadlock-freedom in that sense is a one-line assertion:

```cpp
REQUIRE(result.blocked_executions_explored == 0);
```

Threads that legitimately end a finite program waiting for further input
(server-style receive loops) also classify every terminal as `Blocked`. For
such models, assert instead that specific threads are not blocked by
inspecting the terminal graph for `Block` events belonging to those threads.


`on_terminal_execution` may accept either `TerminalExecutionT<ValueT>` or
`ExplorationGraphT<ValueT>`. Returning `Stop` requests early termination; void
callbacks are treated as `Continue`.

Progress reporting is optional. If `on_progress` is set, DPOR may call it with:

```cpp
enum class ProgressState : std::uint8_t { Running, Stopped, AllExplored };

struct ProgressSnapshot {
  ProgressState state{ProgressState::Running};
  std::chrono::steady_clock::duration elapsed{};
  std::size_t terminal_executions{0};
  std::size_t full_executions{0};
  std::size_t blocked_executions{0};
  std::size_t error_executions{0};
  std::size_t depth_limit_executions{0};
  std::size_t thread_event_limit_executions{0};
  std::size_t max_thread_event_depth{0};
  std::size_t active_workers{0};
  std::size_t max_workers{1};
  std::size_t queued_tasks{0};
  std::size_t max_queued_tasks{0};
  bool counts_exact{true};
};
```

`progress_report_interval > 0` throttles live snapshots to at most one callback
per interval. `progress_report_interval == 0` reports at every internal
progress checkpoint. DPOR always emits one final snapshot with state
`AllExplored` or `Stopped` when exploration returns normally.

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
  std::size_t sync_steps{512};
  std::size_t split_poll_interval_steps{1};
  std::size_t progress_counter_flush_interval{1024};
  std::size_t progress_poll_interval_steps{64};
};
```

`spawn_depth_cutoff` uses the same DPOR tree-depth accounting as `max_depth`.
The current implementation only offers remote work at send backward-revisit
branches.

There used to be a `min_fanout` option here. It was removed: the sole spawn
site passed a literal fanout of `2`, so the option had exactly two reachable
behaviours -- "spawn at send revisits" (`<= 2`) and "never spawn anywhere"
(`> 2`) -- while its name implied a fanout-driven scheduling policy that did
not exist. The true revisit fanout is not cheaply available, because
`next_backward_revisit_child` filters candidates lazily by compatibility, PORF
reachability, and the revisit condition, so counting exactly would cost most of
the revisit work twice. Gating on the cheap upper bound
(`receives_in_destination(send_id).size()`) was rejected as well: a send revisit
with a single child is still worth handing off, since the owning worker keeps
the forward continuation, so an upper-bound gate would only lose parallelism.
Use `max_workers = 1` for serial exploration.

`VerifyResult` reports:

- `VerifyResultKind::AllExplored`
- `VerifyResultKind::Stopped`

and also carries:

- `executions_explored`: total number of published terminal executions
- `full_executions_explored`: number of full executions (every thread completed)
- `blocked_executions_explored`: number of blocked maximal executions
- `error_executions_explored`: number of error executions
- `depth_limit_executions_explored`: number of depth-limit executions
- `thread_event_limit_executions_explored`: number of executions in which the
  engine declined to ask at least one thread for a further event because it sat
  at `max_thread_events`
- `max_thread_event_depth_reached`: the largest per-thread event count, maximized
  over the *published terminal executions* rather than over every transient
  exploration graph -- under an early stop the two only coincide if exploration
  ran to completion. Engine-injected `Block` events count toward it, so a thread
  that blocks at step `max_thread_events - 1` also reports `max_thread_events`;
  the bound itself is never exceeded. Reading it on an unbounded run is how to
  choose a `max_thread_events` value. In parallel mode workers keep it
  thread-local and fold it into the shared counter only when they flush, so the
  live `ProgressSnapshot` value can lag by up to
  `progress_counter_flush_interval` terminals per worker (the same caveat
  `counts_exact` already signals); the final `VerifyResult` value is exact.
- `nd_splits` / `receive_splits`: scheduler diagnostics counting alternatives
  handed to an idle worker from an ND or receive frame. Always `0` in sequential
  mode, and `0` in parallel mode when no worker ever starved, so a test can
  assert that the starvation-splitting path actually engaged rather than
  inferring it from wall-clock.

If `on_terminal_execution` is set, DPOR calls it with each published terminal
execution. Terminal executions are full executions, blocked maximal
executions, error executions, branches truncated by the `max_depth` DPOR
tree-depth limit, and branches truncated by the `max_thread_events` per-thread
event bound. DPOR keeps exploring after error terminals unless the callback
requests `Stop`.

Note that `is_full_execution()` is false for blocked executions: observers
that want every maximal execution must accept both `Full` and `Blocked`.

If `on_progress` is set, sequential exploration reports exact live counts.
Parallel exploration reports exact final counts, and live snapshots may carry
slightly stale terminal counts when `progress_counter_flush_interval > 1`; in
that case `counts_exact` is `false`. When `progress_report_interval > 0`,
parallel workers only poll the clock every `progress_poll_interval_steps`
internal progress checkpoints to keep the hot path cheaper.

With more than one worker, terminal observers may run concurrently and their
order is unspecified. Live progress callbacks run on worker threads but are
serialized with one another. All callbacks and captured state must be safe to
use concurrently; progress and terminal callbacks may overlap.

## Error reporting model

The library separates three kinds of failure, each with its own channel.

### 1. Errors in the system under test: `ErrorLabel`, not exceptions

An error detected in the SUT (a violated protocol invariant, an assertion
failure in handler logic) is an *outcome of the explored interleaving*, not a
failure of the checker. Harnesses report it by returning
`ErrorLabel{message}` from the thread function. DPOR records the error event,
invokes `on_terminal_execution` with kind `TerminalExecutionKind::Error` and
the full counterexample graph, counts it in `error_executions_explored`, and
keeps exploring other interleavings unless the observer requests `Stop`.

The harness is expected to catch SUT-semantic exceptions and convert them into
`ErrorLabel` deterministically. Two obligations remain with the harness:

- *Determinism*: whether the SUT fails must be a deterministic function of
  `(trace, step)`. Catching a nondeterministic failure (OOM, timeout, races in
  the harness) and converting it to `ErrorLabel` masks a contract violation
  and silently breaks DPOR soundness. Let such exceptions escape instead; an
  aborted run is loud, a corrupted exploration is not.
- *Isolation*: catching an exception does not undo what a half-executed
  handler mutated. Run SUT logic against isolated snapshots.

Receive matchers have no `ErrorLabel` channel: they return `bool` and are also
called during consistency checking. When matching defers to SUT logic, fold
rejection into a deterministic `false`.

### 2. Inconsistent candidate graphs: values, not exceptions

Consistency checking returns `ConsistencyResult` issue lists (see below);
inconsistent candidates are silently pruned during exploration. This is
control flow, not an error.

### 3. Exceptions: `dpor::error` hierarchy (`dpor/errors.hpp`)

The library's deliberate contract and invariant exceptions derive from
`dpor::error` (itself a `std::runtime_error`):

- `dpor::internal_error` — a library invariant was violated. Always a bug in
  dpor itself; please report it.
- `dpor::precondition_error` — the caller violated a documented API
  precondition (non-compact or out-of-range thread ids, malformed rf edges,
  out-of-order explicit event indices within a thread, `porf_contains` on a
  cyclic graph, both terminal-observer aliases set, ...).
- `dpor::user_code_error` — an exception escaped a user callback, or a
  callback returned an illegal result (e.g., a thread function returning
  `BlockLabel`, or violating determinism on the blocked-receive reschedule
  path). Carries the callback surface (`kind()`: thread function, receive
  matcher, terminal observer, progress observer), the thread id when known
  (`thread()`), and the original exception (`has_original()`,
  `rethrow_original()`). A `user_code_error` thrown inside a callback passes
  through unwrapped, so the innermost origin wins; any other exception
  crossing the callback boundary is wrapped — including `dpor::*` errors the
  callback itself provoked by calling the library incorrectly.

Any exception reaching the `verify()` / `verify_parallel()` caller is fatal to
the run: exploration stops and the remaining interleavings are not visited.
In parallel mode the first recorded exception is rethrown after workers drain.
Ordinary platform and allocation failures from the C++ runtime (for example,
allocation failure in library-owned storage) are not converted into the
`dpor::error` hierarchy if they reach the caller directly.

### Saving a trace on fatal errors: `on_fatal_error`

`DporConfigT::on_fatal_error` is a diagnostic hook for fatal exceptions raised
*during exploration* — while an in-progress execution exists to report. It is
called at most once, before the exception propagates, with:

```cpp
template <typename ValueT>
struct FatalErrorContextT {
  const model::ExplorationGraphT<ValueT>& graph;  // exploration state at failure
  std::exception_ptr exception;
};
```

Use it to display or save the trace that provoked the failure — the same
graph-printing code used in `on_terminal_execution` works here, and
`model::format_graph(graph[, value_formatter])` from
`include/dpor/model/format.hpp` renders any graph as one event per line in
insertion order. Caveats:

- The graph is a best-effort diagnostic snapshot; after an internal rollback
  failure it may be mid-mutation and inconsistent. The reference is only
  valid during the callback — copy what you need.
- Exceptions thrown by `on_fatal_error` are swallowed so they never mask the
  original failure.
- Fatal failures with no in-progress execution do not invoke it: preconditions
  checked before exploration begins (config validation, thread-id validation,
  the parallel reentrancy check), exceptions from the final progress report
  after exploration ends, and parallel queue/synchronisation plumbing
  failures. These still propagate out of `verify()`/`verify_parallel()` as
  typed exceptions.
- In parallel mode the callback runs on the failing worker's thread and, under
  rare races, may observe a different fatal error than the one
  `verify_parallel()` rethrows.

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

Both insertion paths keep each thread's events strictly increasing in event
index, which is what makes ascending event id equal program order within a
thread. `add_event()` derives the next per-thread index by construction and
appends without further checks, so the monotonic-index validation applies only
to explicit replay/import insertion. `add_event_with_index()` is that path:
gaps are allowed, but an index that repeats or precedes the thread's last index
is a `precondition_error`. A later `add_event()` on the thread resumes *past*
the highest index imported so far rather than backfilling a gap, so importing
index 9 makes the next automatic index 10.

Both insertion paths also reject a thread id above
`dpor::model::kMaxThreadId`. Thread ids
index internal storage directly, so the graph layer caps them independently of
the `Program`-level compact-thread-id validation, which imports bypass.

`po_relation()` validates rather than repairs: if a thread's events are not in
strictly increasing index order it throws `internal_error` instead of sorting
them into a plausible but unfaithful program order.

`add_event()` itself appends without re-checking what it just derived. Build
with `DPOR_VERIFY_GRAPH_INVARIANTS` to verify the append preconditions on every
insertion; the option is on for this project's own build tree and off for
consumers, since the checks sit on the hottest path and one of them is linear
for sparse imported indices.

### `ExplorationGraphT<ValueT>`

`ExplorationGraphT` wraps `ExecutionGraphT` with DPOR-specific state such as
rollback support, per-thread derived metadata, and cached `(po ∪ rf)+`
reachability. Events are append-only, so insertion order is identical to
ascending event id and is materialized on demand rather than stored twice.

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

`porf_contains()` requires an acyclic graph and throws `dpor::precondition_error`
on causal cycles.

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
`po` and `rf` directly. The lazy adapters store references to their operands,
so their inputs must outlive the adapter; rvalue operands are rejected at
compile time.

## Practical notes

- Prefer `execution.graph` in `on_terminal_execution` observers; it exposes the
  most useful query surface for inspecting explored executions.
- Prefer `ExecutionGraphT` when manually constructing a graph for tests or
  checking consistency outside the DPOR engine.
- Predicate-based receives are part of the intended integration model for
  existing systems. They are not limited to finite value sets.
- Soundness depends on determinism and isolation. Mutable captures,
  time-dependent matchers, and leaked side effects can invalidate exploration
  guarantees. Parallel runs additionally require concurrency-safe callbacks.
