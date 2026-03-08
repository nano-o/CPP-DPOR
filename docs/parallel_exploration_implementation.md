# Parallel Exploration

This document describes the parallel DPOR implementation that currently exists
in `include/dpor/algo/dpor.hpp`.

It is no longer a design plan for multiple scheduler variants. The current
implementation supports one parallel scheduling strategy: a bounded global
queue with work-first local recursion.

## Public API

Parallel exploration is opt-in through `verify_parallel()`:

```cpp
template <typename ValueT>
VerifyResult verify_parallel(
    const DporConfigT<ValueT>& config,
    ParallelVerifyOptions options = {});
```

The sequential `verify()` entry point is unchanged.

`ParallelVerifyOptions` currently contains:

```cpp
struct ParallelVerifyOptions {
  std::size_t max_workers{0};
  std::size_t max_queued_tasks{0};
  std::size_t spawn_depth_cutoff{0};
  std::size_t min_fanout{2};
  std::size_t sync_steps{0};
};
```

Current option semantics:

- `max_workers == 0` means use `std::thread::hardware_concurrency()`, falling
  back to `1` if the runtime reports `0`.
- `max_queued_tasks == 0` derives to `max_workers * 2`.
- `spawn_depth_cutoff == 0` means no depth cutoff.
- `min_fanout` gates whether a branch is even considered for remote execution.
- `sync_steps == 0` enables the strict result-publication path.
- `sync_steps > 0` reduces synchronization overhead but weakens early-stop
  semantics after an error.

## High-Level Shape

The parallel executor uses a single global task queue protected by
`queue_mutex_` and `queue_cv_`.

Each queued task owns:

- an `ExplorationGraphT<ValueT>`
- a recursion `depth`
- an `ExplorationTaskMode` (`Visit` or `VisitIfConsistent`)

No mutable exploration graph state is shared across workers.

The main thread participates as a worker. `verify_parallel()` seeds the queue
with the empty graph, starts `max_workers - 1` helper threads, then runs the
same `worker_loop()` on the calling thread.

## Scheduling Policy

The implemented strategy is work-first:

1. Keep the first child of a branch local.
2. On ND and receive branches, compute a branch-local enqueue budget from the
   current queue occupancy before copying any later sibling.
3. Attempt to enqueue later siblings only while that budget remains and
   `can_spawn(...)` passes.
4. If enqueue fails, recurse locally instead.

`can_spawn(child_depth, fanout)` currently requires:

- `max_workers > 1`
- stop not requested
- `fanout >= min_fanout`
- `child_depth <= spawn_depth_cutoff` when a cutoff is configured

The queue is only a backlog buffer. Workers always prefer continuing local
rollback-based recursion over waiting for queue space.

For ND and receive branches, the enqueue budget is a best-effort snapshot:

- `min(total_children - 1, available_queue_slots)` at branch entry
- still subject to the final locked `try_enqueue(...)` check

This avoids copying child graphs for siblings that cannot fit in the queue
under the current backlog pressure.

An exact-reservation variant for ND/receive branches was also tested: reserve
queue capacity first, eagerly materialize and enqueue exactly those reserved
siblings, then recurse locally. On the timeout benchmark (`participants=4`,
`--parallel --max-workers 8`) that was consistently worse than the current
snapshot-budget heuristic:

- default queue budget: `12815.865 ms -> 17183.547 ms`
- `--max-queued-tasks 1`: `13380.854 ms -> 17829.186 ms`

The likely reason is that eager reserved enqueue pays remote copy/materialize
cost before the local work-first path can make progress.

## Branch Handling

The implementation parallelizes only at existing DPOR branch points.

### ND Branches

- The first choice stays local.
- Later choices may be enqueued as owned child graphs, but only up to the
  branch-local enqueue budget derived from visible queue capacity.

### Receive Branches

- The compatible unread sends are computed once.
- Each matching `(recv, send_id)` child becomes an independent graph.
- The non-blocking bottom branch is treated as another child.
- As with ND, later siblings only take the copy-and-enqueue path while the
  branch-local enqueue budget remains non-zero.

### Send Branches

- The worker appends the send locally and keeps the forward continuation local.
- Backward-revisit children are streamed one by one out of
  `for_each_backward_revisit_child(...)`.
- Each revisited graph may be enqueued; if not, it is explored locally.

This is the most important spawn boundary in the current implementation because
revisit children are already fully materialized owned graphs.

### Block / Reschedule Paths

- Plain `Block` append continues locally.
- `reschedule_blocked_receive_if_enabled_impl(...)` already materializes an
  owned unblocked graph and explores it directly on the current worker.

## Ownership Invariant

Parallel correctness depends on one rule:

- failed remote handoff must not consume the owned child graph

The current implementation preserves that invariant explicitly. When enqueue is
attempted through `try_enqueue_owned_task(...)`, the helper:

1. moves the graph into a temporary task
2. calls `executor.try_enqueue(task)`
3. if enqueue fails, moves the graph back out of `task`
4. falls back to local recursion on the original child

This matters most on send-revisit children. Earlier versions moved a revisit
graph into the enqueue attempt and then explored the moved-from object locally
on failure, which produced severe parallel-path regressions.

## Result Semantics

`VerifyResultKind` is the same as in sequential mode:

- `AllExecutionsExplored`
- `ErrorFound`
- `DepthLimitReached`

Current behavior differs by `sync_steps`.

### Strict Mode: `sync_steps == 0`

- `publish_complete_execution()` and `publish_error_execution()` serialize
  through `publication_mutex_`.
- Exactly one error terminal is published.
- No complete execution is counted or observed after the stop flag has been
  committed by an error path.

This is the default.

### Relaxed Mode: `sync_steps > 0`

- workers cache stop-flag reads for `sync_steps` polling calls
- execution counts are accumulated in thread-local state and flushed later
- more than one worker may independently reach an error terminal
- complete executions may still be counted after the first error is committed

This mode exists only to reduce synchronization overhead.

## Ordering And Observers

Parallel exploration does not preserve sequential DFS observation order.

What is preserved:

- execution counts in the all-executions-explored case
- explored execution sets, as validated by tests against sequential/oracle runs
- exact sequential observation order when `max_workers == 1`

`on_execution` may be invoked concurrently in parallel mode. Callback code must
therefore be thread-safe in addition to being deterministic.

## Worker / Callback Assumptions

The implementation assumes:

- thread functions are deterministic for the same exploration state
- thread functions are safe to call concurrently from multiple workers
- any callback logic used by receive predicates or observers is side-effect free
  or isolated

These are required for DPOR correctness, not just performance.

## Current Tests

The parallel test coverage in `tests/dpor_test.cpp` currently checks:

- `verify_parallel()` with one worker matches sequential execution order exactly
- parallel execution sets match sequential and oracle execution sets on a mixed
  branching program
- clean stop behavior when sibling branches race to an error
- depth-limit reporting
- correctness under tiny queue budgets and high fanout
- enqueue-fallback ownership preservation through a focused regression test

## Known Limitations

Current non-goals and limitations:

- only one scheduler strategy is implemented
- no work stealing or per-worker deques
- no attempt to preserve DFS order once `max_workers > 1`
- queue bounds only limit queued snapshots, not worker-local recursion state
- revisit children are still materialized eagerly enough to pay graph-copy cost
- `sync_steps > 0` deliberately weakens error-stop semantics

## Benchmark Surface

The benchmark CLIs currently expose the queue-backlog tuning knobs that map
directly to `ParallelVerifyOptions`:

- `--parallel`
- `--max-workers`
- `--max-queued-tasks`
- `--spawn-depth-cutoff`
- `--min-fanout`

The benchmark harness does not currently expose `sync_steps`.

## Practical Summary

The implemented parallel mode is:

- a separate `verify_parallel()` entry point
- a bounded central queue plus worker pool
- work-first local recursion with optional sibling handoff
- value-based graph handoff only
- correctness-first, not order-preserving

That is the code shape the rest of the repository should treat as the current
parallel exploration contract.
