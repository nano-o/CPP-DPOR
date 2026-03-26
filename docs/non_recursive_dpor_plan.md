# Non-Recursive DPOR Exploration Implementation

This document describes the non-recursive DPOR exploration engine that landed
in `include/dpor/algo/dpor.hpp`.

Historically this file was an implementation plan. It now documents the
behavior and structure of the code that replaced the old recursive traversal.

## Why The Change Was Made

The previous DPOR core used C++ recursion in two places:

- ordinary in-place DFS over one mutable `ExplorationGraphT`
- recursive descent into distinct owned child graphs

The second case was the real stack-growth problem. Backward revisits and
blocked-receive reschedules can recurse into a smaller child graph while the
parent call chain remains live. In practice, that means C++ stack depth can
grow far beyond the size of the currently active execution graph.

The new implementation removes that recursion and replaces it with an explicit
stack machine.

## High-Level Result

The core traversal is now implemented by:

- `DepthFirstExplorer<ValueT, ExecutorT>`

It is shared by:

- `verify()`
- `detail::visit(...)`
- `detail::visit_if_consistent(...)`
- parallel worker-owned task processing through the existing executor path

The public exploration APIs and high-level semantics stay the same. The main
change is internal control flow: the search is now iterative rather than
recursive.

## Main Data Structures

### `ExplorationFrame`

`ExplorationFrame` is the small continuation object used for in-place
exploration within one mutable graph.

Each frame stores only:

- `kind`
- `dpor_tree_depth`
- `mode` (`Visit` or `VisitIfConsistent`)
- `checkpoint`
- `cursor`
- `event_id`
- `flag`

This is deliberate. The frame does not cache large branch payloads such as ND
choice vectors or compatible-send lists. On resume, the explorer rolls the
graph back to the stored checkpoint, recomputes the current next event, and
uses the scalar cursor to skip already-explored siblings.

That keeps frame memory small and keeps the hot path rollback-based.

### `ExplorationContext`

`ExplorationContext` owns the graph root for one local exploration context plus
its frame stack.

There are two kinds of contexts:

- a borrowed root graph supplied by the caller
- an owned child graph created for a branch that cannot be reached by rollback
  from the current graph

Each context contains:

- either `borrowed_graph` or `owned_graph`
- `frames`

The explorer maintains a vector of contexts. In the common case there is only
one current mutable graph. Additional contexts appear only when the algorithm
must suspend one graph and temporarily explore a distinct owned child graph.

## Frame Kinds That Landed

The landed frame kinds are:

- `Enter`
- `ExitLinearChild`
- `ResumeNd`
- `ResumeReceive`
- `ResumeSendRevisits`

The original design sketch included a separate send-forward resume phase. The
implementation ended up simpler: once send revisits are exhausted, the same
frame is converted to `ExitLinearChild` and the ordinary post-send child
`Enter` frame is pushed immediately. That removed one frame kind without
changing behavior.

## Control Flow

### Entry

`Enter` is the common entry point for exploring a graph state.

At entry the explorer:

1. runs the consistency check if the frame is in `VisitIfConsistent` mode
2. reports progress through the executor
3. honors stop requests
4. checks `max_depth` against `dpor_tree_depth`
5. computes the next event

If the frame is consistent and should continue, the frame mode is normalized to
`Visit` so later resumption does not re-run the consistency check unnecessarily.

### Full terminal

If there is no next event, the explorer first tries blocked-receive
reschedule. If no reschedule child exists, it publishes a full execution and
pops the frame.

### Error

Errors remain a local temporary mutation:

1. checkpoint
2. append the error event
3. publish the error terminal
4. rollback immediately
5. pop the frame

### Empty ND and `Block`

Both are handled as linear children:

1. checkpoint
2. append the event
3. convert the current frame to `ExitLinearChild`
4. push a child `Enter` at `dpor_tree_depth + 1`

When the child finishes, `ExitLinearChild` rolls back the checkpoint and pops.

### ND with choices

Non-empty ND uses `ResumeNd`.

The frame stores:

- the checkpoint before the ND append
- a scalar `cursor` identifying the next choice to explore

On each resume:

1. rollback to the checkpoint
2. recompute the current ND label from the restored graph
3. if `cursor` is past the last choice, pop
4. otherwise append the chosen ND value, increment the cursor, and push the
   child `Enter` frame

### Receive

Receives use `ResumeReceive`.

The frame stores:

- the checkpoint before the receive append
- a scalar `cursor` over compatible sends
- `flag`, which records whether the non-blocking bottom branch is still
  available

On each resume:

1. rollback to the checkpoint
2. recompute the current receive label
3. rescan unread sends and skip to the indexed compatible source
4. if a real send source is selected, append the receive, bind `rf`, and push
   a child `VisitIfConsistent` frame
5. if no send branch remains but the non-blocking bottom branch is still
   enabled, append the receive, bind bottom `rf`, clear the flag, and push a
   child `VisitIfConsistent` frame
6. otherwise pop

This keeps receive frames small and avoids storing send lists in the frame.

### Send

Sends are handled in two phases:

1. append the send once and switch to `ResumeSendRevisits`
2. when revisit children are exhausted, push the ordinary forward child

`ResumeSendRevisits` stores:

- the checkpoint before the send append
- `event_id` for the live send event
- `cursor` over destination receives
- `flag`, which caches whether the executor is allowed to spawn remote tasks

The frame uses `next_backward_revisit_child(...)` to stream revisit children
one at a time. For each revisit child:

- if remote enqueue is allowed and succeeds, the frame stays in place and
  continues with the next child later
- otherwise the child is explored locally by pushing a new owned context in
  `VisitIfConsistent` mode

When no revisit child remains, the same frame is converted to
`ExitLinearChild`, and the ordinary post-send child `Enter` frame is pushed at
`dpor_tree_depth + 1`.

This preserves the previous search order:

- revisit children first
- ordinary send-forward continuation after revisits

## Backward Revisit Enumeration

The old code eagerly walked revisit children through recursive control flow.
The landed code instead splits revisit generation into two helpers:

- `next_backward_revisit_child(...)`
- `for_each_backward_revisit_child(...)`

`DepthFirstExplorer` uses `next_backward_revisit_child(...)` directly so it can
resume revisit enumeration from a scalar receive index stored in the frame.

`for_each_backward_revisit_child(...)` remains as a thin utility used by the
standalone `detail::backward_revisit(...)` helper.

## Blocked-Receive Reschedule

Blocked-receive reschedule is the main area where the landed code intentionally
deviates from the strictest possible single-graph design.

### What the implementation does

At a full-terminal point, the explorer calls
`find_blocked_receive_reschedule_child(...)`.

That helper:

1. scans threads in the existing deterministic thread-id order
2. finds threads whose last event is `Block`
3. constructs `unblocked_graph` by removing that `Block`
4. re-runs the thread function on the unblocked graph
5. requires the result to be a blocking receive with at least one compatible
   unread send
6. returns the first such `unblocked_graph`

If a reschedule child is found, `handle_enter_frame()`:

1. records the current `dpor_tree_depth`
2. pops the current `Enter` frame
3. pushes the `unblocked_graph` as a new owned context in `Visit` mode at the
   same `dpor_tree_depth`

This is tail-like, but it is not an in-place graph swap.

### Why it was implemented this way

Replacing the current graph in place would be unsafe when there are parent
frames below the current `Enter` frame. Those parent frames hold checkpoints
taken on the original graph, and `rollback()` requires the checkpoint to match
the graph being rolled back.

So the final implementation keeps blocked-reschedule as an owned child context.
That avoids recursion and preserves correctness, but it can temporarily keep a
parent graph and a smaller unblocked child graph alive at the same time.

### How this differs from the ideal memory story

The normal local DFS path uses one mutable graph plus small checkpoint-based
frames. Blocked-reschedule is one of the exceptions where an additional owned
graph may remain live temporarily.

That is a deliberate compromise:

- correctness and checkpoint validity were prioritized for the first landing
- the remaining memory hotspot is narrow and explicit

## DPOR Tree Depth

The old internal variable name `depth` was misleading after the recursion
removal, so the landed code renames the internal plumbing to
`dpor_tree_depth`.

`dpor_tree_depth` means logical depth in the DPOR search tree. It does not
mean:

- current C++ stack depth
- current frame-stack depth
- current graph size

The implemented accounting is:

- ordinary forward children use `dpor_tree_depth + 1`
- backward-revisit children use `dpor_tree_depth + 1`
- blocked-reschedule keeps the same `dpor_tree_depth`

This is the quantity used by:

- `DporConfigT::max_depth`
- `ParallelVerifyOptions::spawn_depth_cutoff`

## Root Graph Restoration And Exceptions

The borrowed root graph passed into `detail::visit(...)` and
`detail::visit_if_consistent(...)` is still restored on return.

That behavior now comes from the iterative unwind rules:

- linear and resumable frames roll back their checkpoints before popping
- owned child contexts are destroyed when their frame stacks empty
- error terminals roll back immediately after publication

`DepthFirstExplorer::run_loop()` also catches exceptions, unwinds all remaining
frames and contexts, and then rethrows. That preserves the previous rollback
guarantee for callers even in exceptional paths.

## Sequential And Parallel Integration

The same `DepthFirstExplorer` is used in both sequential and parallel
exploration.

The parallel ownership model is unchanged:

- queued tasks still own their graphs
- mutable graph state is not shared across workers

The spawn policy is also unchanged:

- only send backward-revisit children may be enqueued remotely
- ND and receive siblings remain local

If enqueue fails, the owned graph is restored to the caller and explored
locally.

## Memory Characteristics

The landed design keeps memory low on the common path by relying on:

- one mutable graph per local context
- small per-frame checkpoints and cursors
- recomputation of branch data on resume instead of caching vectors

The main cases that can retain multiple full graphs locally are:

- local backward-revisit fallback
- blocked-receive reschedule

Those are now isolated exceptions rather than the default exploration
mechanism.

## Validation Performed For The Landing

The implementation was validated with:

- `ctest --preset debug -R dpor_dpor_test`
- `ctest --preset asan`
- `scripts/run_tsan.sh`

The landing also added deep linear regression coverage in
`tests/dpor_test.cpp` for:

- `verify()`
- `verify_parallel()` with `max_workers = 1`

The documented two-phase-commit DPOR benchmark smoke workload was also used as
a sanity check that one-worker parallel mode stayed close to sequential mode.

## Remaining Trade-Offs

The recursion is gone from the core DPOR exploration engine, but one trade-off
remains explicit: local exploration can still temporarily retain multiple full
graphs when the algorithm must suspend one graph and visit another owned child
graph.

Today that applies to:

- local backward revisits
- blocked-receive reschedules

If future work needs stricter "memory proportional only to the currently
active graph" behavior, those two paths are the right place to focus. The main
iterative frame machine is already structured so that such follow-up work can
be targeted without reintroducing recursion.
