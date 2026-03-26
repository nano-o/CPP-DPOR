# Non-Recursive DPOR Exploration Plan (Proposed)

This document is the implementation plan for removing recursion from the DPOR
exploration engine in `include/dpor/algo/dpor.hpp`.

The immediate motivation is correctness and robustness: the current recursive
search can overflow the C++ call stack on deep explorations. The plan below is
intended to be concrete enough that the next agent can implement it directly.

## Problem Statement

Today, the search uses recursion in two different ways:

- in-place DFS recursion on a single mutable `ExplorationGraphT<ValueT>`
- recursion into a distinct owned child graph via `process_owned_task(...)`

The second case is the one that makes stack depth diverge from the size of the
currently-visited graph. This happens primarily in:

- backward revisit from the send branch
- blocked-receive reschedule

In those paths, the child graph can be smaller than the parent, but the parent
frames remain live on the C++ stack while the child is explored.

## Goals

1. Remove recursion from both `verify()` and `verify_parallel()`.
2. Preserve DPOR semantics and coverage.
3. Preserve the current sequential exploration order unless a targeted test
   proves that a change is intentional and safe.
4. Keep the hot path rollback-based and avoid full graph copies for ordinary
   ND / receive / block / send-forward exploration.
5. Keep per-frame memory small. A frame should store checkpoints and scalar
   cursors, not whole graph snapshots.
6. Preserve current observer behavior:
   - terminal observers see the terminal graph before rollback
   - `detail::visit(...)` still leaves the caller graph restored
7. Preserve current depth accounting:
   - ordinary forward steps increment `depth`
   - backward-revisit children inherit `depth + 1`
   - blocked-reschedule keeps the same `depth`

## Non-Goals

- changing Must semantics
- redesigning `restrict_masked(...)`, revisit conditions, or consistency logic
- widening the parallel scheduler policy beyond the current send-revisit spawn
  point
- relying on compiler tail-call optimization as the fix

## Memory Constraints

The important distinction is:

- `ExplorationGraphT::Checkpoint` is small and cheap
- retaining multiple full `ExplorationGraphT` objects is not

Therefore:

- a `FrameStack` of checkpoints is acceptable
- a generic stack of copied graphs for every branch is not

This plan keeps the normal in-place DFS path on one mutable graph and uses
small continuation frames. The places where multiple owned graphs may remain
live are:

- local backward-revisit fallback
- blocked-receive reschedule after popping the current `Enter` frame

The latter is necessary because replacing the current graph in place would
invalidate any parent checkpoints still live in the same context. That is a
conscious compromise for the first landing, because it preserves correctness
and reviewability.

If stricter "memory proportional to the currently active graph only" behavior
becomes mandatory, the follow-up work should attack local backward revisits
and blocked-reschedule replay specifically, not the whole frame design.

## High-Level Design

### 1. `FrameStack`: continuation state within one graph

Introduce an explicit stack of continuation frames that replaces recursive
`visit_impl(...)` / `visit_if_consistent_impl(...)` calls on a single mutable
graph.

A frame should contain only:

- a frame kind / phase tag
- `depth`
- `ExplorationTaskMode`
- an `ExplorationGraphT<ValueT>::Checkpoint`
- a small number of scalar cursors / flags
- event ids or thread ids where needed

The frame should not cache large vectors such as:

- ND choice copies
- compatible-send lists
- destination-receive lists

Instead, on resume, recompute the branch data from the rolled-back graph and
skip already-explored siblings by cursor index. This is safe because thread
callbacks are already required to be deterministic and side-effect-free for the
same graph state.

### 2. `ContextStack`: owned graph roots only

Keep a narrow context abstraction for the cases where exploration switches to a
distinct owned graph that cannot be reached via rollback from the current one.

A context owns:

- `ExplorationGraphT<ValueT> graph`
- its local `FrameStack`

Why a context exists at all:

- backward-revisit children are materialized as restricted+rewired owned graphs
- if a revisit child is explored locally, the parent send continuation still
  has to survive until that child returns
- blocked-reschedule children are materialized as restricted owned graphs, and
  replacing the current graph in place would invalidate parent rollback
  checkpoints when the current `Enter` frame is not the only frame in the
  context

What should not use a new context:

- ordinary ND / receive / block / send-forward recursion

### 3. One driver shared by sequential and parallel

Introduce one internal iterative driver, for example:

- `DepthFirstExplorer<ValueT, ExecutorT>`

The driver should be shared by:

- `verify()`
- `detail::visit(...)`
- `detail::visit_if_consistent(...)`
- `ParallelExecutor::process_task(...)`

This avoids creating a new divergence between sequential and parallel code
paths while removing recursion.

## Recommended Frame Kinds

Use a small tagged union or equivalent compact structure. The exact names can
change, but the state split should stay close to:

- `Enter`
- `ResumeNd`
- `ResumeReceive`
- `ResumeSendRevisits`
- `ResumeSendForward`
- `ExitLinearChild`

Notes:

- `Visit` vs `VisitIfConsistent` should be a frame mode, not separate recursive
  entry points.
- `ExitLinearChild` is the common "child finished, now rollback my checkpoint
  and pop" state for simple one-child cases.

## Control Flow by Event Kind

### Entry / common checks

At frame entry:

1. call `executor.maybe_report_progress()`
2. honor `stop_requested()`
3. if mode is `VisitIfConsistent`, run the consistency check and prune on
   failure
4. honor `max_depth`
5. compute the next event

### Full terminal

If there is no next event:

1. try blocked-receive reschedule
2. if reschedule succeeds, pop the current `Enter` frame and push an owned
   child context rooted at the `unblocked_graph` in `Visit` mode at the same
   `depth`
3. otherwise publish a full execution and pop

### Error

Error remains a local temporary mutation:

1. take a checkpoint
2. append the error
3. publish the error terminal
4. rollback immediately
5. pop

### Empty ND

Treat this as a linear child:

1. take a checkpoint
2. append the ND event
3. convert the current frame to `ExitLinearChild`
4. push a child `Enter` frame with `depth + 1`

### ND with choices

Use one frame with a scalar cursor:

1. save a checkpoint before the ND append
2. store `tid` and `next_choice_index`
3. on each resume:
   - rollback to the checkpoint
   - recompute the ND label from the graph
   - if no more choices remain, pop
   - append the next chosen ND event
   - push the child frame

Do not keep a copied vector of choices in the frame.

### Receive

Use the same pattern as ND:

1. save a checkpoint before the receive append
2. store `tid`, `next_send_index`, and whether the bottom branch remains
3. on each resume:
   - rollback to the checkpoint
   - recompute the receive label from the graph
   - rescan unread sends and skip to the indexed compatible sibling
   - for a real send source: append receive, set `rf`, push child
   - for the non-blocking bottom branch: append receive, set bottom `rf`, push
     child
   - once all siblings are exhausted, pop

This intentionally trades some recomputation for bounded frame memory.

### Send

Send has two logically separate continuations:

1. backward-revisit children
2. the ordinary forward continuation after the send

Recommended shape:

1. take a checkpoint before the send append
2. append the send once on the current context graph
3. convert the frame to `ResumeSendRevisits`
4. in `ResumeSendRevisits`, stream revisit children one by one out of
   `for_each_backward_revisit_child(...)`
5. for each child:
   - if remote enqueue is allowed and succeeds, continue the same frame
   - otherwise push a new owned context for local exploration
6. after revisit children are exhausted, transition to `ResumeSendForward`
7. in `ResumeSendForward`, push the ordinary child `Enter` frame at `depth + 1`
8. when that child returns, rollback the pre-send checkpoint and pop

Important:

- preserve current behavior where backward revisits are considered before the
  normal post-send continuation
- do not enqueue ND / receive siblings; keep the current send-only spawn policy

### Block

Treat block as a linear child:

1. checkpoint
2. append `Block`
3. convert to `ExitLinearChild`
4. push child `Enter` at `depth + 1`

## API Boundary Behavior

`detail::visit(...)` and `detail::visit_if_consistent(...)` accept a caller
graph by reference and today leave it restored after return.

Preserve that behavior explicitly:

- keep an outer rollback guard at the wrapper boundary, or
- otherwise guarantee that the root borrowed graph is restored on both normal
  return and exceptions

Do not make callers depend on moved-from or permanently mutated input graphs.

## Parallel Path

The parallel executor should keep its current ownership model:

- queued tasks own their graphs
- no mutable graph state is shared across workers

The new driver runs inside `ParallelExecutor::process_task(...)`.

Current spawn policy remains:

- only send backward-revisit children may be enqueued remotely
- all other branching remains local

This keeps the current performance rationale from
`docs/parallel_exploration_implementation.md`.

## Incremental Implementation Plan

### Step 1: Add regression tests before the refactor

Add or extend tests covering:

- a deep linear program in `verify()` that would previously risk stack
  overflow
- the same program in `verify_parallel(max_workers = 1)`
- current rollback visibility for full and error terminals
- exact sequential vs one-worker-parallel execution order
- current graph restoration after enqueue rejection

The deep test does not need to be huge in debug builds, but it should be large
enough to catch recursive regressions if recursion accidentally remains.

### Step 2: Introduce the frame data structure and the iterative driver

Create the non-recursive driver in `include/dpor/algo/dpor.hpp`, but do not
switch all callers yet.

Start by supporting:

- `Enter`
- `ExitLinearChild`
- common terminal publication

This lets the driver handle:

- depth-limit terminal
- full terminal without reschedule
- error terminal
- block
- empty ND

### Step 3: Fold `VisitIfConsistent` into frame mode

Once the iterative entry path works for simple cases, move the consistency
check into the frame mode so that:

- `visit_impl(...)`
- `visit_if_consistent_impl(...)`

stop being distinct recursive engines.

### Step 4: Port ND and receive branching

Move ND and receive from recursion plus `ScopedRollback` to cursor-based frame
continuations.

Guardrails:

- keep frame memory small
- recompute siblings from the graph on resume
- preserve sibling order exactly

### Step 5: Port blocked-reschedule as a tail-like owned child

Replace the current recursive reschedule path with:

- build `unblocked_graph`
- pop the current `Enter` frame
- push the unblocked graph as a new owned context at the same depth

This still removes recursion, but it does not eliminate all "smaller graph,
deeper context stack" behavior yet.

### Step 6: Port send handling and narrow local owned-context use to revisit children

Convert the send branch to the explicit send frame phases described above.

At the end of this step:

- local backward-revisit fallback and blocked-reschedule tail branches are the
  only places that still need a new owned context
- ordinary forward DFS should no longer recurse anywhere

### Step 7: Switch all entry points to the new driver

After the iterative driver covers all cases:

- make `verify()` use it
- make `detail::visit(...)` use it
- make `detail::visit_if_consistent(...)` use it
- make `ParallelExecutor::process_task(...)` use it

### Step 8: Remove the recursive helpers

Delete or reduce to thin wrappers:

- `recurse_graph(...)`
- `process_owned_task(...)`
- `explore_branch(...)`
- recursive-only `visit_impl(...)` control flow

Leave helper extraction in place if it still improves readability, but the
driver itself should be iterative.

## Validation Plan

Minimum required validation:

- `cmake --preset debug`
- `cmake --build --preset debug`
- `ctest --preset debug`
- `cmake --preset asan`
- `cmake --build --preset asan`
- `ctest --preset asan`
- `scripts/run_tsan.sh`

Targeted performance sanity checks:

- compare sequential runtime on a deep linear program before and after
- run the documented 2PC timeout benchmark smoke test
- confirm `verify_parallel(max_workers = 1)` still matches sequential exactly

## Rejected Approaches

Rejected for this landing:

- relying on compiler tail-call optimization
- a generic stack of copied `ExplorationTask` graphs for all sequential
  exploration
- storing whole sibling vectors in frames
- enqueuing ND / receive siblings for parallel exploration

## Open Follow-Up

The remaining memory hotspot after this landing will be nested local
backward-revisit fallback, because that still retains parent and child owned
graphs simultaneously.

If that becomes a practical problem, the next design target should be:

- replace local revisit contexts with a replay / recompute scheme, or
- intentionally change local revisit traversal order to avoid suspended parent
  graphs

That should be treated as a follow-up optimization after the non-recursive
landing is correct and fully tested.
