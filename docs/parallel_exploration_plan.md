# Parallel Exploration Plan

This document proposes an optional parallel DPOR search mode. It is intentionally separate from Phase 6 in [docs/performance_optimization_plan.md](/home/nano/code/dpor/docs/performance_optimization_plan.md): the goal here is to add safe parallel tasking on top of the current worker-local rollback design, not to redesign revisit internals first.

## Recommendation

A thread pool is the right overall shape, but not as "spawn every recursive `visit()` call".

The current engine has two different execution modes already implied by its ownership model:

- worker-local recursion, where `visit()` mutates one `ExplorationGraphT` and uses rollback
- task-spawn boundaries, where a worker must hand off an owned graph snapshot

That leads to one clear rule:

- inside one worker: keep the current by-reference `visit()` recursion and `ScopedRollback`
- across workers: enqueue owned `ExplorationGraphT` snapshots plus `depth`

The recommended scheduler policy is work-first:

- the current worker keeps one child locally
- sibling children are spilled to the pool only when there is capacity

This preserves the cheap rollback-based local path and avoids sharing mutable graph state across workers.

## Current Engine Constraints

The current code shape matters here:

- `detail::visit()` mutates `ExplorationGraphT&` and relies on rollback for forward branching
- `ProgramT`, `thread_ids`, and the non-mutable parts of `DporConfigT` are read-only across recursion
- `backward_revisit()`, `restrict()`, `with_rf()`, and blocked-receive rescheduling already materialize independent graphs
- `VerifyResult` and `on_execution` are currently single-threaded and deterministic in DFS order
- `ThreadFunctionT` callbacks are required to be deterministic already; in parallel mode they must also be safe to invoke concurrently

The existing rollback refactor is therefore a good fit for parallel search, but only if cross-worker handoff stays value-based.

## Design Goals

- keep sequential `verify()` behavior unchanged
- add optional parallel exploration rather than replacing the current path
- never share mutable `ExplorationGraphT` state across workers
- bound memory growth from queued graph snapshots
- preserve correctness and oracle agreement
- avoid making Phase 6 a prerequisite

## Non-Goals For The First Landing

- exact sequential DFS observation order in parallel mode
- zero-copy cross-worker handoff
- work stealing or lock-free scheduling on day one
- parallelizing mutation inside one recursion stack

## API Shape

There are two reasonable public surfaces:

1. Add config fields such as:

```cpp
std::size_t max_workers{1};
std::size_t max_queued_tasks{0};     // 0 => derive from max_workers
std::size_t spawn_depth_cutoff{0};   // 0 => no cutoff
bool parallel_experimental{false};
```

2. Add a separate `verify_parallel(config)` entry point and keep `verify(config)` fully sequential.

Recommendation: prefer a separate `verify_parallel()` first. That keeps semantic caveats isolated and avoids quietly widening the contract of the existing `verify()` API.

## Core Internal Types

One conservative internal shape is:

```cpp
template <typename ValueT>
struct ExplorationTask {
  model::ExplorationGraphT<ValueT> graph;
  std::size_t depth;
};

struct SharedSearchState {
  std::atomic<bool> stop_requested{false};
  std::atomic<std::size_t> executions_explored{0};
  std::mutex result_mutex;
  std::optional<VerifyResult> first_error;
};

template <typename ValueT>
class ParallelExecutor {
 public:
  VerifyResult run(const DporConfigT<ValueT>& config);

 private:
  void worker_loop();
  void process_task(ExplorationTask<ValueT> task);
  bool try_enqueue(ExplorationTask<ValueT> task);
};
```

All queued tasks own their graph value. No task may retain references into another worker's graph or rollback history.

## Scheduling Strategy

Use work-first DFS:

1. When a branch produces `N` child subtrees, the current worker keeps the first child locally.
2. Remaining children are enqueued only if:
   - `max_workers > 1`
   - the queue is below budget
   - the depth is below a configurable cutoff, if one is enabled
3. If enqueue fails or the pool is saturated, fall back to local recursion.

Why this is the right first implementation:

- it preserves the current fast local path
- it limits graph copying to actual spawn boundaries
- it avoids flooding the pool with tiny tasks

Start with a single global queue protected by `std::mutex` + `std::condition_variable`. If profiling later shows scheduler contention, move to per-worker deques plus work stealing as a follow-up.

## Duplicate Exploration

No global deduplication layer is planned for the first landing.

The design assumption is that Must's revisiting construction already gives unique revisit sources, so parallel execution order does not create new duplicate exploration as long as the implementation stays faithful to that structure. In other words:

- each worker owns one parent task
- each child graph is emitted exactly once by that parent
- parallelism changes when children run, not which children exist

Under that assumption, if two different workers ever materialize the same revisit child, that indicates a bug or a deviation from the Must algorithm rather than something that should be handled by an extra dedup layer.

This assumption should still be checked empirically:

- `max_workers == 1` through the parallel path must match sequential execution counts and execution sets exactly
- small parallel-vs-sequential agreement tests should compare execution signatures as sets
- oracle-backed tests should continue to agree

## Spawn Boundaries In The Current `visit()` Shape

Parallelism should be added by enumerating owned child graphs at the branch points that already exist today.

### ND branch

- current worker keeps one choice locally
- sibling choices become child snapshots

### Receive branch

- build the compatible send list once
- each `(recv, send_id)` branch becomes independent after append plus `set_reads_from`
- the non-blocking bottom branch is another independent child

### Send branch

- keep the forward continuation on the current worker after appending the send
- let `backward_revisit()` materialize revisited child graphs and emit them to the scheduler one by one
- do not try to share the worker's mutated post-send graph across threads
- do not batch all revisited graphs before dispatch; streaming them reduces memory pressure and lets idle workers start earlier

### Block branch

- append block and usually keep exploring locally
- this branch has low fan-out, so it is unlikely to be a useful split point early on

### `reschedule_blocked_receive_if_enabled()`

- the unblocked graph is already a standalone snapshot and can become a task

This implies one refactor before real parallelism:

- split branch construction from branch execution
- let `backward_revisit()` emit owned revisited graphs to a callback or sink instead of recursing directly in all cases
- each emitted revisit graph must already be a fully independent snapshot produced by `restrict()` plus `with_rf()`
- the sink should be non-blocking from `backward_revisit()`'s perspective: it should either take ownership immediately or tell the caller to run the child locally rather than waiting for queue space
- the parent graph remains mutated with the fresh send visible until revisit enumeration is done; the parent rollback happens only after all revisit children have been emitted or retained for local execution

## Result And Cancellation Semantics

This is the hard part. The thread-pool mechanics are straightforward; preserving current sequential semantics is not.

Recommended first implementation:

- parallel mode is opt-in and explicitly experimental
- the `AllExecutionsExplored` case must preserve the exact execution count
- on `ErrorFound`, a worker sets a shared stop flag once it publishes an error
- workers check the stop flag before expensive expansion and before publishing terminal results
- workers should also check the stop flag inside branch loops and inside `backward_revisit()` before materializing more children, so cancellation remains responsive
- plain sequential `verify()` semantics must remain unchanged

There are two viable policies:

### Policy 1: simpler first landing

- in parallel mode, the winning error execution/message is whichever worker publishes first
- `on_execution` may run concurrently on worker threads with unspecified order
- `executions_explored` after an error is not promised to match sequential DFS

### Policy 2: stricter later landing

- add a coordinator that publishes completed subtrees in deterministic branch order
- preserve sequential first-error semantics and observer order
- much more complex; treat this as a follow-up rather than a prerequisite for basic speedup

Recommendation: choose Policy 1 first, but only behind a separate `verify_parallel()` or explicit experimental config gate. Keep ordinary `verify()` fully deterministic.

## Observer Handling

In `verify_parallel()`, `config.on_execution` may be called concurrently from multiple worker threads, with unspecified order.

That implies a stronger API contract for parallel mode:

- observers must be thread-safe and reentrant
- observers must tolerate concurrent invocation
- observers must not assume DFS order
- if ordered publication is ever needed, it should be a later coordinator-based feature rather than part of the first landing


## Thread Safety Requirements

Parallel exploration raises the bar for user-provided callbacks:

- `ThreadFunctionT` must remain deterministic and side-effect free
- `ThreadFunctionT` must also be safe to invoke concurrently from multiple worker threads
- receive compatibility predicates must also be thread-safe
- `on_execution` must be thread-safe and reentrant in parallel mode
- captured mutable state shared across thread functions becomes a correctness bug in parallel mode

Worker threads should share one read-only `ProgramT` and therefore one shared set of `ThreadFunctionT` objects by const reference. There is no plan to clone thread functions per worker. That means mutable lambda captures are shared mutable state unless the user arranges their own synchronization or makes the callbacks effectively immutable.

This needs to be documented next to the new API/config surface.

## Implementation Plan

### Phase 1: API And Semantic Guardrails

1. Add an explicit parallel entry point or config fields.
2. Keep sequential `verify()` unchanged.
3. Document the first-landing semantics:
   - experimental mode
   - winning error in parallel mode is unspecified if multiple branches race
   - `on_execution` may run concurrently and must be thread-safe

### Phase 2: Internal Scheduler Skeleton

1. Add `ExplorationTask`, `SharedSearchState`, and `ParallelExecutor`.
2. Implement a bounded central task queue with worker threads, mutex, and condition variable.
3. Add stop propagation and worker shutdown.
4. Route only the root task through the new executor first so worker startup/shutdown and result aggregation can be tested before branch splitting is enabled.

### Phase 3: Refactor `visit()` Into "Enumerate Children Or Recurse"

1. Extract helpers that can produce owned child graphs for:
   - ND choices
   - receive/send matches
   - non-blocking bottom branches
   - blocked-receive reschedule
2. Refactor `backward_revisit()` so it can emit revisited child graphs to a callback or sink.
3. Preserve the existing local rollback path for the worker that keeps a child local.

### Phase 4: Enable Work-First Parallel Splitting

1. At each useful branching point, keep one child local.
2. Enqueue sibling children when the queue has capacity and stop is not requested.
3. Add conservative heuristics:
   - do not spawn below a minimum fan-out
   - do not spawn past a configurable depth cutoff
   - cap queued tasks to control memory

### Phase 5: Result Aggregation And Error Cancellation

1. Count completed executions atomically.
2. Publish the first observed error once and set `stop_requested`.
3. Ensure workers drop queued work promptly after stop, with stop checks at task entry, inside branch loops, and inside `backward_revisit()`.
4. Make sure no worker publishes terminal results after the stop decision if that would violate the chosen experimental semantics.

### Phase 6: Observer And Determinism Follow-Up

1. Decide whether ordered callbacks or deterministic first-error publication are ever needed.
2. If they are, design a coordinator for ordered publication.
3. Only after that consider making parallelism part of the ordinary `verify()` surface.

## Testing Plan

### Correctness

- keep all existing sequential tests running unchanged with one worker
- add parallel-vs-sequential agreement tests on small programs with:
  - ND branching
  - blocking receives
  - non-blocking bottom branches
  - backward revisit
  - blocked-receive reschedule
- compare final execution signatures as sets, not callback order
- compare against the oracle on small bounded cases

### Concurrency

- repeated stress runs of the existing randomized DPOR tests in parallel mode
- a test where multiple workers race to report an error and exploration still terminates cleanly
- a test that saturated branching obeys queue and task-budget limits
- a test that no shared graph mutation occurs across tasks, backed first by ownership discipline and later by TSAN if a TSAN build is added
- if a TSAN build/preset is added, the work-first branch-splitting landing should be TSAN-clean before it is considered done

### API And Semantics

- `verify()` remains deterministic and unchanged
- parallel `on_execution` may execute concurrently with unspecified order
- `max_workers == 1` through the parallel path matches sequential results exactly

## Performance Expectations

Parallel exploration can speed up branch-heavy workloads, but it will not be free:

- each spawned task reintroduces graph snapshot copying at the split boundary
- revisit-heavy workloads may scale poorly until revisit materialization is revisited later
- scheduler overhead should be acceptable if tasks are coarse, but some spawned subtrees will still prune quickly, so this must be measured rather than assumed
- queue limits only bound queued snapshots; total memory also includes each worker's local DFS/rollback state, so memory budgeting must consider both components

## Current Benchmark Findings

Early measurements on the timeout-inclusive two-phase commit benchmark show that
the current parallel implementation does not yet have a good default split
policy.

- `participants=3`, `iterations=1`, `--no-crash`:
  - sequential: about `549 ms`
  - parallel with `max_workers=1`: about `585 ms`
  - parallel with `max_workers=4` and the current defaults
    (`spawn_depth_cutoff=0`, `min_fanout=2`): still running after about `20 s`,
    manually stopped
  - parallel with `max_workers=4` plus either `spawn_depth_cutoff=1` or
    `spawn_depth_cutoff=2`: about `590 ms`
  - parallel with `max_workers=4`, `spawn_depth_cutoff=0`,
    `min_fanout=4`: about `590 ms`

- `participants=4`, `iterations=1`, `--no-crash`:
  - sequential: about `80.2 s`
  - parallel with `max_workers=1`: about `80.4 s`
  - parallel with `max_workers=4` and current defaults used about four cores in
    a short probe, but did not finish within `30 s`
  - more conservative settings that avoided the blow-up only used about one
    core during the first `10-15 s` of the same probe, so they were unlikely to
    deliver meaningful speedup

These results are useful because they isolate the problem:

- the parallel executor path itself is close to neutral when no real spawning
  happens (`max_workers=1`)
- the default split policy is too aggressive on this workload
- once spawning is constrained enough to avoid the blow-up, the search no
  longer exposes enough parallel work to keep multiple workers busy

The current working diagnosis is:

- spawning on binary branches at arbitrary depth is too fine-grained for this
  state space
- graph snapshot copying and queue/scheduler overhead dominate any useful
  parallelism under the current defaults
- a shallow depth cutoff or higher fan-out gate suppresses that overhead, but
  also suppresses most available parallel work

That means the next step should be measurement-driven tuning rather than blind
default changes. In particular, the executor should grow low-overhead counters
for:

- spawn attempts
- successful enqueues
- enqueue-failure fallbacks to local recursion
- tasks processed
- maximum queue depth
- branch-type breakdowns for ND, receive, and send-revisit splits

Those counters should be used to drive a capped benchmark matrix before claiming
any general speedup on this workload.

Success criteria for the first landing:

- no correctness regressions
- measurable wall-clock win on branch-heavy examples with more than one worker
- bounded memory growth under queue pressure
- sequential performance with one worker unchanged or near-unchanged

## Recommendation Summary

A thread pool is worth doing, but the right shape is:

- local rollback-based DFS inside a worker
- owned graph snapshots across worker boundaries
- work-first scheduling, not recursive firehose spawning
- explicit experimental semantics at first, especially around `ErrorFound` and `on_execution`

That fits the current engine instead of fighting it.
