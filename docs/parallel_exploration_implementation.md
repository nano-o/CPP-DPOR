# Parallel Exploration

This document describes the parallel DPOR implementation that currently exists
in `include/dpor/algo/dpor.hpp`.

It is no longer a design plan for multiple scheduler variants. The current
implementation supports one parallel scheduling strategy: a bounded global
queue with work-first local exploration.

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
  std::size_t sync_steps{512};
  std::size_t progress_counter_flush_interval{1024};
  std::size_t progress_poll_interval_steps{64};
};
```

Current option semantics:

- `max_workers == 0` means use `std::thread::hardware_concurrency()`, falling
  back to `1` if the runtime reports `0`.
- `max_queued_tasks == 0` derives to `max_workers * 2`.
- `spawn_depth_cutoff == 0` means no DPOR tree-depth cutoff.
- `min_fanout` gates whether a branch is even considered for remote execution.
- `sync_steps == 0` enables the strict result-publication path.
- `sync_steps > 0` reduces synchronization overhead but weakens early-stop
  semantics after a callback requests stop.
- the default is `sync_steps == 512`
- `progress_counter_flush_interval == 0` resolves to a small default.
- `progress_counter_flush_interval > 1` batches worker-local terminal counts
  before flushing them into shared progress counters.
- `progress_poll_interval_steps == 0` and `== 1` both poll at every progress
  checkpoint.
- larger `progress_poll_interval_steps` values reduce clock reads and report
  claim attempts for throttled progress reporting.

## High-Level Shape

The parallel executor uses a single global task queue protected by
`queue_mutex_` and `queue_cv_`.

Each queued task owns:

- an `ExplorationGraphT<ValueT>`
- a DPOR tree depth
- an `ExplorationTaskMode` (`Visit` or `VisitIfConsistent`)

No mutable exploration graph state is shared across workers.

The main thread participates as a worker. `verify_parallel()` seeds the queue
with the empty graph, starts `max_workers - 1` helper threads, then runs the
same `worker_loop()` on the calling thread.

## Scheduling Policy

The implemented strategy is work-first, with parallel spawning restricted to
send backward-revisit branches:

1. ND and receive branches explore all children locally on one mutable graph
   through rollback-based iterative frames.
2. Send branches keep the forward continuation local. Backward-revisit children
   — which are materialized one at a time by
   `next_backward_revisit_child(...)` inside the iterative explorer — may be enqueued for remote
   execution if `can_spawn(...)` passes. If enqueue fails, they are explored
   locally instead.

`can_spawn(child_dpor_tree_depth, fanout)` currently requires:

- `max_workers > 1`
- stop not requested
- `fanout >= min_fanout`
- `child_dpor_tree_depth <= spawn_depth_cutoff` when a cutoff is configured

The queue is only a backlog buffer. Workers always prefer continuing local
rollback-based exploration over waiting for queue space.

### Why only send branches spawn

ND and receive branches use in-place mutation with explicit checkpoints and
iterative rollback-based frames to avoid graph copies. Enqueuing a sibling from
these branches requires copying the parent graph, which is pure overhead. Send
backward-revisit children, by contrast, are already fully materialized owned
graphs, so enqueuing them costs no additional copy.

On the 4-participant no-crash 2PC timeout benchmark, restricting parallelism to
send branches is neutral to ~8% faster across 1-20 workers compared to also
enqueuing ND/receive siblings, with identical execution counts (7,262,928).

Earlier variants that enqueued ND/receive siblings used an `enqueue_budget`
mechanism to limit graph copies by snapshotting queue occupancy at branch entry.
That machinery (including the locked capacity snapshot in
`ParallelExecutor::enqueue_budget()`) has been removed in favor of the simpler
send-only policy.

### Previously tested ND/receive enqueue strategies

An exact-reservation variant for ND/receive branches was tested: reserve queue
capacity first, eagerly materialize and enqueue exactly those reserved siblings,
then recurse locally. On the timeout benchmark (`participants=4`, `--parallel
--max-workers 8`) that was consistently worse than the snapshot-budget heuristic
it aimed to replace:

- default queue budget: `12815.865 ms -> 17183.547 ms`
- `--max-queued-tasks 1`: `13380.854 ms -> 17829.186 ms`

One important caveat: that prototype tracked reserved credits separately, but
the ordinary enqueue path did not subtract those credits. Other workers could
therefore still consume queue capacity that had been "reserved", so the result
applies to that imperfect implementation rather than to a fully enforced
reservation scheme.

A follow-up hard-reservation variant was also tested, where ordinary enqueue
treated reserved credits as consumed capacity until they were used or released.
That was still slower than the snapshot-budget heuristic:

- default queue budget: `12815.865 ms -> 16673.359 ms`
- `--max-queued-tasks 1`: `13380.854 ms -> 17895.274 ms`

Taken together, the likely issue is not just imperfect reservation accounting.
The eager reserved-enqueue shape itself appears to pay remote
copy/materialize/scheduling cost before the local work-first path can make
progress.

## Branch Handling

The implementation parallelizes only at existing DPOR branch points.

### ND Branches

- All choices are explored locally by iterative rollback-based frames on the
  parent graph.

### Receive Branches

- The compatible unread sends are rescanned on resume from the rolled-back
  parent graph.
- Each matching `(recv, send_id)` child and the non-blocking bottom branch are
  explored locally by iterative rollback-based frames.

### Send Branches

- The worker appends the send locally and keeps the forward continuation local.
- Backward-revisit children are streamed one by one out of
  `next_backward_revisit_child(...)` as the `ResumeSendRevisits` frame advances.
- Each revisited graph may be enqueued for remote execution; if enqueue fails,
  it is explored locally as an owned child context.

This is the sole parallel spawn point. Revisit children are already fully
materialized owned graphs, so enqueuing them incurs no additional copy.

### Block / Reschedule Paths

- Plain `Block` append continues locally.
- Blocked-receive reschedule materializes an owned unblocked graph and explores
  it on the current worker as a tail-like owned child context at the same DPOR
  tree depth.

## Ownership Invariant

Parallel correctness depends on one rule:

- failed remote handoff must not consume the owned child graph

The current implementation preserves that invariant explicitly. When enqueue is
attempted through `try_enqueue_owned_task(...)`, the helper:

1. moves the graph into a temporary task
2. calls `executor.try_enqueue(task)`
3. if enqueue fails, moves the graph back out of `task`
4. falls back to local exploration on the original child

This matters most on send-revisit children. Earlier versions moved a revisit
graph into the enqueue attempt and then explored the moved-from object locally
on failure, which produced severe parallel-path regressions.

## Result Semantics

`VerifyResultKind` is the same as in sequential mode:

- `AllExplored`
- `Stopped`

Current behavior differs by `sync_steps`.

### Strict Mode: `sync_steps == 0`

- `publish_full_execution()`, `publish_depth_limit_execution()`, and
  `publish_error_execution()` serialize
  through `publication_mutex_`.
- Terminal-count updates are serialized against stop checks.
- Live progress snapshots can still lag worker-local terminal counts if
  `progress_counter_flush_interval > 1`.
- `on_terminal_execution` callbacks are still invoked outside
  `publication_mutex_`, so another worker may still publish a terminal that
  already passed the stop check before a callback's `Stop` request is
  committed.

This mode is available explicitly, but it is no longer the default.

### Relaxed Mode: `sync_steps > 0`

- workers cache stop-flag reads for `sync_steps` polling calls
- worker-local terminal counts are flushed into shared progress counters only
  when the configured batching threshold is reached
- workers only poll the clock for interval-throttled progress reporting every
  `progress_poll_interval_steps` progress checkpoints
- additional terminal executions may still be counted after the first callback
  requests stop

This mode exists only to reduce synchronization overhead.

## Ordering And Observers

Parallel exploration does not preserve sequential DFS observation order.

What is preserved:

- execution counts in the all-executions-explored case
- explored execution sets, as validated by tests against sequential/oracle runs
- exact sequential observation order when `max_workers == 1`

`on_terminal_execution` may be invoked concurrently in parallel mode. Callback
code must therefore be thread-safe in addition to being deterministic.
As in sequential mode, published terminal executions include full executions,
error executions, and branches cut off by the `max_depth` DPOR tree-depth
limit.

`on_progress` may also be invoked from worker threads. Its snapshots report:

- elapsed time since exploration start
- total/full/error/depth-limit terminal counts
- current `active_workers` and queued-task count
- configured worker and queue capacities
- whether the live counts are exact

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
- queue bounds only limit queued snapshots, not worker-local exploration state
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
- work-first local iterative exploration with send-revisit handoff
- value-based graph handoff only
- correctness-first, not order-preserving

That is the code shape the rest of the repository should treat as the current
parallel exploration contract.
