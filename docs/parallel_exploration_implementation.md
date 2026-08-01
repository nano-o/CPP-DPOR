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
  std::size_t sync_steps{512};
  std::size_t split_poll_interval_steps{1};
  std::size_t progress_counter_flush_interval{1024};
  std::size_t progress_poll_interval_steps{64};
};
```

Current option semantics:

- `max_workers == 0` means use `std::thread::hardware_concurrency()`, falling
  back to `1` if the runtime reports `0`.
- `max_queued_tasks == 0` derives to `max_workers * 2`.
- `spawn_depth_cutoff == 0` means no DPOR tree-depth cutoff.
- `sync_steps == 0` enables the strict result-publication path.
- `sync_steps > 0` reduces synchronization overhead but weakens early-stop
  semantics after a callback requests stop.
- the default is `sync_steps == 512`
- `split_poll_interval_steps` controls how often an ND or receive frame
  consults the idle-worker count before offering an alternative for remote
  execution. `0` and `1` both mean check every branch, and that is the default:
  batching measured slower on all three benchmark workloads. It only affects
  when work is offered, never which executions are explored.
- `progress_counter_flush_interval == 0` resolves to `1024`.
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

### Wake protocol

Workers wait on `queue_cv_` with the predicate

```
stop_requested_ || search_complete_ || !task_queue_.empty()
```

Each disjunct has exactly one owner responsible for waking waiters, and nothing
else broadcasts:

| Disjunct becomes true | Woken by |
|---|---|
| `!task_queue_.empty()` | `try_enqueue()`, one `notify_one()` per successful push |
| `stop_requested_` | `request_stop()` / `record_exception()`, `notify_all()` |
| `search_complete_` | the worker that drains the last task, `notify_all()` |

A queued task can therefore never strand a sleeping worker even though
`notify_one()` is lost when no thread happens to be waiting. Any worker that
finishes a task re-evaluates the predicate under `queue_mutex_` before it is
able to block, so the last active worker either finds the queue non-empty and
takes the task, or finds it empty and is by definition the one that sets
`search_complete_`. Only that transition needs a broadcast.

Completing a task used to broadcast unconditionally. That woke O(workers)
threads per completed task against a fixed amount of useful work, so scheduling
cost grew with worker count and the wake-ups were nearly all spurious: the
predicate could not have newly become true for any of them. On a three-node SCP
externalize workload that cost 46% of total CPU in the kernel at 32 workers and
made 32 workers slower than one.

## Scheduling Policy

The implemented strategy is work-first. Send backward-revisit branches spawn
unconditionally; ND and receive branches spawn only under worker starvation:

1. Send branches keep the forward continuation local. Backward-revisit children
   — which are materialized one at a time by
   `next_backward_revisit_child(...)` inside the iterative explorer — may be
   enqueued for remote execution if `can_spawn(...)` passes. If enqueue fails,
   they are explored locally instead.
2. ND and receive branches explore their children locally on one mutable graph
   through rollback-based iterative frames, *except* that a single alternative
   may be handed to a parked worker when
   `should_split_to_idle_worker(...)` passes. See "Starvation splitting" below.

`can_spawn(child_dpor_tree_depth)` currently requires:

- `max_workers > 1`
- stop not requested
- `child_dpor_tree_depth <= spawn_depth_cutoff` when a cutoff is configured

A `min_fanout` option used to add a fourth condition, `fanout >= min_fanout`.
It was removed. The sole call site passed a literal `2` rather than the real
number of revisit children, so the only reachable behaviours were "spawn at
send revisits" (`min_fanout <= 2`) and "never spawn" (`> 2`) -- a boolean whose
name implied a fanout-driven policy that did not exist. Feeding it the real
count is not cheap, since `next_backward_revisit_child` filters candidates
lazily and counting exactly would repeat most of the revisit work; and gating
on the cheap upper bound `receives_in_destination(send_id).size()` would only
lose parallelism, because a revisit branch with a single child is still worth
handing off while the owning worker keeps the forward continuation. Use
`max_workers = 1` for serial exploration.

The queue is only a backlog buffer. Workers always prefer continuing local
rollback-based exploration over waiting for queue space.

### Why sends spawn freely and ND/receive branches do not

ND and receive branches use in-place mutation with explicit checkpoints and
iterative rollback-based frames to avoid graph copies. Enqueuing a sibling from
these branches requires copying the parent graph. Send backward-revisit
children, by contrast, are already fully materialized owned graphs, so enqueuing
them costs no additional copy — which is why sends spawn whenever they can and
ND/receive branches need a reason to pay.

### Starvation splitting

ND and receive frames split at the point where the handler has already called
`graph.rollback(frame.checkpoint)`, so the graph sits in the parent state.
Applying one alternative to a copy reproduces exactly the child the local path
would have built:

| Alternative | Applied to the copy | Enqueued as |
|---|---|---|
| ND choice `i` | `add_event(thread_id, nd_label with value = choices[i])` | `Visit` |
| Receive candidate `i` | `add_event` then `set_reads_from(recv, candidate[i])` | `VisitIfConsistent` |

The modes match what the local handlers push, so the remote `Enter` frame runs
the identical consistency check the local child would have. There is no new task
variant and no resumable frame: `run()` builds the task's `Enter` frame and
`handle_enter_frame` takes its own checkpoint. That matters, because
`ExplorationGraphT`'s copy constructor deliberately does not copy the undo logs,
so a copied graph could not be rolled back to an ancestor checkpoint anyway.

Three rules keep this correct and cheap:

- **The cursor advances only on a successful handoff.** If both sides claimed an
  alternative the branch would run twice; if neither did it would be lost. Either
  error can hide inside equal aggregate counts, which is why the tests compare
  execution *sets*.
- **The non-blocking bottom branch is never split.** It is always the last
  alternative in a receive frame, so handing it off would leave the worker with
  nothing, and keeping it local means `frame.flag` is consumed exactly once by
  its owner — there is no split/local ownership race over it to get wrong. For
  the same reason a split only happens while at least one further alternative
  would remain local.
- **The gate short-circuits before touching shared state.** The common
  "nobody is idle" path is a plain member compare plus a thread-local
  increment. `can_spawn()` is consulted only after the idle check passes,
  because it calls `stop_requested()` and would otherwise advance the shared
  stop-poll cadence on every branch.

That last point is not a micro-optimization. An earlier version of this gate
called `can_spawn()` first and cost ~7% on the 2PC benchmark even though
splitting fired only ~1,000 times against 7,262,928 executions.

### Measured effect

Paired same-session medians against the identical engine with starvation
splitting removed, on a 16-physical-core SMT2 host, `stellar-core`'s
`scp-dpor-investigation`:

| Workload | workers | send-only | with splitting |
|---|---|---|---|
| SCP, rf/ND-heavy (1,278,277 executions) | 8 | 8.16s | 4.97s |
| | 16 | 6.78s | 3.03s |
| | 32 | 6.83s | 2.58s |
| SCP, send-heavy (5,600,446 executions) | 16 | 21.73s | 20.24s |
| | 32 | 18.74s | 17.20s |

The rf/ND-heavy workload is the one that starved: its queue sat empty with 4-20
of 32 workers active, because rf-choice and ND subtrees produced no tasks. The
send-heavy workload already kept its queue full, so it gains only a few percent.

On the 4-participant no-crash 2PC timeout benchmark the difference is inside the
measurement noise. At 8 workers, 11 alternated repetitions gave send-only 7619ms
and splitting 7752ms (1.018x), while a *copy of the send-only binary measured
against itself in the same session* came out at 0.966x — a larger deviation than
the change being tested. Splitting was slower in 5 of 11 paired repetitions.
Execution counts were identical (7,262,928) at every worker count.

Two earlier prototypes that enqueued ND/receive siblings were rejected (below).
Both were *eager*: they reserved and materialized siblings whether or not any
worker was idle. Paying the copy only when a peer is actually parked is a
different cost profile, and the earlier measurements were also taken against the
pre-fix scheduler, whose cost grew with worker count.

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

- Choices are explored locally by iterative rollback-based frames on the parent
  graph.
- While a peer is parked and a further choice would remain local, one choice may
  instead be applied to a copy of the parent graph and enqueued as `Visit`.

### Receive Branches

- The compatible unread sends are rescanned on resume from the rolled-back
  parent graph.
- Each matching `(recv, send_id)` child and the non-blocking bottom branch are
  explored locally by iterative rollback-based frames.
- While a peer is parked and further work would remain local, one
  `(recv, send_id)` candidate may instead be applied to a copy and enqueued as
  `VisitIfConsistent`. The bottom branch is never handed off.

### Send Branches

- The worker appends the send locally and keeps the forward continuation local.
- Backward-revisit children are streamed one by one out of
  `next_backward_revisit_child(...)` as the `ResumeSendRevisits` frame advances.
- Each revisited graph may be enqueued for remote execution; if enqueue fails,
  it is explored locally as an owned child context.

Revisit children are already fully materialized owned graphs, so enqueuing them
incurs no additional copy. This is the only spawn point that fires
unconditionally; ND and receive frames spawn only under starvation.

### Block / Reschedule Paths

- Plain `Block` append continues locally.
- Blocked-receive reschedule first performs its trace, next-step, and
  unread-send viability checks on the original graph. Only a viable candidate
  materializes an owned unblocked graph, which is explored on the current
  worker as a tail-like owned child context at the same DPOR tree depth.

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

- `publish_terminal_execution()` serializes through `publication_mutex_`.
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
code and captured state must therefore be thread-safe.
As in sequential mode, published terminal executions include full executions,
blocked maximal executions, error executions, and branches cut off by the
`max_depth` DPOR tree-depth limit.

`on_progress` may also be invoked from worker threads. Its snapshots report:

- elapsed time since exploration start
- total/full/blocked/error/depth-limit terminal counts
- current `active_workers` and queued-task count
- configured worker and queue capacities
- whether the live counts are exact

## Worker / Callback Assumptions

The implementation assumes:

- thread functions and receive matchers are deterministic for the same
  exploration state and do not leak side effects between calls
- stateful model adapters operate on isolated snapshots
- all user callbacks and captured state are safe to use concurrently from
  multiple workers; observers may synchronize and update their own state

These are required for DPOR correctness, not just performance.

## Current Tests

The parallel coverage in `tests/dpor_test.cpp` and `tests/errors_test.cpp`
currently checks:

- `verify_parallel()` with one worker matches sequential execution order exactly
- parallel execution sets match sequential and oracle execution sets on a mixed
  branching program
- all error terminals are reported (without stopping exploration) when sibling
  branches race to an error
- best-effort early stop when a terminal observer returns `Stop`
- depth-limit reporting
- blocked-terminal classification
- serialized live progress reporting with exact final counts
- callback exception propagation and fatal-trace reporting
- rejection of reentrant `verify_parallel()` calls from worker callbacks
- correctness under tiny queue budgets and high fanout
- enqueue-fallback ownership preservation through a focused regression test

## Known Limitations

Current non-goals and limitations:

- only one scheduler strategy is implemented
- no work stealing or per-worker deques
- no attempt to preserve DFS order once `max_workers > 1`
- queue bounds only limit queued snapshots, not worker-local exploration state
- revisit children are still materialized eagerly enough to pay graph-copy cost
- `sync_steps > 0` deliberately weakens early-stop semantics
- starvation splitting hands off one alternative per visit to the frame, so a
  wide ND or receive frame is drained one copy at a time rather than in a range
- the idle-worker signal is a hint: a stale read only means an opportunity is
  taken or missed, never a change to the explored set

## Benchmark Surface

The benchmark CLIs currently expose the tuning knobs that map directly to
`ParallelVerifyOptions`:

- `--parallel`
- `--max-workers`
- `--max-queued-tasks`
- `--spawn-depth-cutoff`
- `--progress-counter-flush-interval`
- `--progress-poll-interval-steps`

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
