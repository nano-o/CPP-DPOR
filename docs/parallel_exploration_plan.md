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
- let `backward_revisit()` materialize revisited child graphs and emit them to the scheduler
- do not try to share the worker's mutated post-send graph across threads

### Block branch

- append block and usually keep exploring locally
- this branch has low fan-out, so it is unlikely to be a useful split point early on

### `reschedule_blocked_receive_if_enabled()`

- the unblocked graph is already a standalone snapshot and can become a task

This implies one refactor before real parallelism:

- split branch construction from branch execution
- let `backward_revisit()` emit owned revisited graphs to a callback or sink instead of recursing directly in all cases

## Result And Cancellation Semantics

This is the hard part. The thread-pool mechanics are straightforward; preserving current sequential semantics is not.

Recommended first implementation:

- parallel mode is opt-in and explicitly experimental
- the `AllExecutionsExplored` case must preserve the exact execution count
- on `ErrorFound`, a worker sets a shared stop flag once it publishes an error
- workers check the stop flag before expensive expansion and before publishing terminal results
- plain sequential `verify()` semantics must remain unchanged

There are two viable policies:

### Policy 1: simpler first landing

- in parallel mode, the winning error execution/message is whichever worker publishes first
- `on_execution` is either disabled or serialized with unspecified order
- `executions_explored` after an error is not promised to match sequential DFS

### Policy 2: stricter later landing

- add a coordinator that publishes completed subtrees in deterministic branch order
- preserve sequential first-error semantics and observer order
- much more complex; treat this as a follow-up rather than a prerequisite for basic speedup

Recommendation: choose Policy 1 first, but only behind a separate `verify_parallel()` or explicit experimental config gate. Keep ordinary `verify()` fully deterministic.

## Observer Handling

Do not call `config.on_execution` concurrently from worker threads in the first landing unless the API is changed to require thread-safe observers.

Recommended staging:

1. Initial parallel mode rejects `on_execution` with `std::invalid_argument`.
2. Follow-up option A: serialize observer calls behind a mutex and document unspecified order.
3. Follow-up option B: build a deterministic publication coordinator if ordered callbacks matter.

## Thread Safety Requirements

Parallel exploration raises the bar for user-provided callbacks:

- `ThreadFunctionT` must remain deterministic and side-effect free
- `ThreadFunctionT` must also be safe to invoke concurrently from multiple worker threads
- receive compatibility predicates must also be thread-safe
- captured mutable state shared across thread functions becomes a correctness bug in parallel mode

This needs to be documented next to the new API/config surface.

## Implementation Plan

### Phase 1: API And Semantic Guardrails

1. Add an explicit parallel entry point or config fields.
2. Keep sequential `verify()` unchanged.
3. Document the first-landing semantics:
   - experimental mode
   - `on_execution` rejected
   - winning error in parallel mode is unspecified if multiple branches race

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
3. Ensure workers drop queued work promptly after stop.
4. Make sure no worker publishes terminal results after the stop decision if that would violate the chosen experimental semantics.

### Phase 6: Observer And Determinism Follow-Up

1. Decide whether unordered serialized callbacks are acceptable.
2. If not, design a coordinator for ordered publication.
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

### API And Semantics

- `verify()` remains deterministic and unchanged
- phase-1 parallel mode rejects `on_execution`
- `max_workers == 1` through the parallel path matches sequential results exactly

## Performance Expectations

Parallel exploration can speed up branch-heavy workloads, but it will not be free:

- each spawned task reintroduces graph snapshot copying at the split boundary
- revisit-heavy workloads may scale poorly until revisit materialization is revisited later
- scheduler overhead will dominate if tasks are too fine-grained

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
