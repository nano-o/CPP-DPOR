# Performance Optimization Plan

This is the single source of truth for DPOR performance optimization work. It covers diagnosis, phased implementation, measurement results, and future plans.

## Constraint

Different parallel `visit()` calls must not share mutable exploration state.

That implies two distinct execution contexts:

- worker-local recursion, where temporary mutation plus rollback is allowed
- task-spawn boundaries, where a worker must hand off an isolated snapshot

Today, the by-value `visit()` / `visit_if_consistent()` style is already the natural task-spawn boundary interface: a caller can hand an owned graph to a worker without sharing mutable state. If local rollback is introduced later, that should remain an internal optimization inside one worker rather than a change to the spawn-boundary ownership model.

## Diagnosis

1. graph copying and allocator churn are the largest costs
2. consistency checking and cycle detection rebuild too much state
3. backward revisit amplifies graph-materialization cost
4. the timeout adapter's string parsing is real, but secondary to DPOR-core costs

## Guiding Principles

1. Preserve correctness first.
2. Preserve parallel-task isolation at all times.
3. Land low-risk wins before invasive refactors.
4. Do not optimize by assuming stronger invariants than the current API actually guarantees.

## Design Decisions

Accepted:

- use a structured integer or enum `ValueT` in the 2PC timeout benchmark
- replace clearly dense event-ID-indexed maps with flat vectors where semantics allow it
- remove duplicate cycle work carefully
- reserve obvious hot vectors
- clean up move/copy usage in `visit()` where ownership already allows it
- distinguish worker-local branching from parallel task boundaries
- add explicit task-snapshot semantics if local mutation is introduced
- treat `restrict()` and `with_rf()` as special revisit-heavy cases
- stage the refactor so rollback machinery is validated before recursion changes

Rejected:

- replacing `used_event_indices_by_thread_` with dense bitsets or `vector<bool>`
- using `graph.has_causal_cycle()` as a drop-in replacement for checker cycle validation if it changes malformed-graph behavior from issue reporting to exceptions
- applying incremental PORF maintenance outside the append-only forward path
- sharing mutable graph state across parallel workers

## Roadmap

### Phase 0: Baseline and Guardrails

Before making core changes:

- keep the current perf trace as a baseline
- keep oracle-backed correctness as the semantic guardrail
- compare not just runtime, but also allocation-heavy symbols and execution counts

### Phase 1: Cheap, Example-Local Wins

Commits: `f51e312`, `38909e3`

Land the least risky benchmark-specific changes first:

1. Replace the timeout example's `std::string` message `ValueT` with a structured enum or integer encoding.
2. Remove text deserialization from the simulation path.
3. Keep the public library API unchanged; limit this phase to the example and benchmark wiring.

Expected result:

- less string allocation and copying
- lower replay overhead in `run_and_capture()` and `sim_receive()`

### Phase 2: Safe Representation Cleanups In Core Types

Commits: `b4c3be8`, `607127a`, `d50b6c7`

Apply dense storage only where the current semantics support it:

1. `insertion_position_` can become a dense vector indexed by `EventId`.
2. `reads_from_` can become a dense vector of optional or sentinel-valued sources indexed by receive `EventId`.
3. `thread_state_` and `next_event_index_by_thread_` can move to vectors only if thread IDs are made explicitly dense inside the implementation.
4. If that densification is done, it should be owned by the graph layer, not by callers. The implementation should maintain an internal `ThreadId -> dense_index` mapping and keep the external `ThreadId` API unchanged.
5. Keep `used_event_indices_by_thread_` sparse and set-based because replay/import supports sparse caller-provided indices.
6. Land trivial ownership cleanups in `visit()` here as well: missed moves and avoidable copies in branches that already own their graph value.

Expected result:

- fewer hash-node allocations
- cheaper copies even before any structural branching refactor

Implementation note:

- The first Phase 2 landing densified `insertion_position_`, `reads_from_`, and some `visit()` ownership cleanups (last-branch move optimization).
- Thread-indexed state was densified in a follow-up: `thread_state_`, `next_event_index_by_thread_`, `PorfCache::thread_clock_index`, `ProgramT::threads` (via a new `ThreadMapT<T>` container), and `derive_thread_event_sequences()`. Rather than an internal `ThreadId -> dense_index` mapping layer, the approach validates at exploration start that thread IDs form a compact contiguous 0-based or 1-based range (`validate_compact_thread_ids()`), then uses `ThreadId` directly as a vector index throughout.
- Phase 2 is now complete. All event-ID-indexed and thread-ID-indexed maps in the core types have been replaced with dense vectors.

### Phase 3: Remove Duplicate Cycle Work Without Losing Diagnostics

Commits: `0269722`, `9f3d012`

(duplicate cycle work between the consistency checker and PORF cache)

The current checker and PORF cache both detect cycles. The merged plan keeps the optimization target but preserves existing issue-reporting behavior.

Acceptable implementations:

1. validate malformed reads-from endpoints first, then reuse a non-throwing cycle query
2. or add a non-throwing cycle-only path that shares the same graph-derived state as the PORF cache
3. or pass a precomputed cycle result into the checker after the checker has completed the validations that currently produce `ConsistencyIssueCode`s

Do not:

- replace structured issue reporting with exceptions

Expected result:

- lower cost in `visit_if_consistent()`
- reduced rebuilding of PO/cycle state

Implementation note:

- The checker was split internally into a `validate_graph` pass (endpoint/RF validation, issue collection) and a separate cycle decision. The existing `check(const ExecutionGraphT&)` overload is unchanged; a new `check(const ExplorationGraphT&)` overload was added for the DPOR hot path and oracle helpers.
- The initial implementation routed all cycle queries through `graph.has_causal_cycle()`, which triggers `ensure_porf_cache()`. This caused a measurable regression on the 2PC timeout benchmark: the `p4 --no-crash` benchmark moved from `136.93s` on the Phase 2 baseline commit (`d50b6c7`) to `151.23s` on the first Phase 3 commit (`0269722`). Cyclic graphs that would be pruned were now paying for full vector-clock computation instead of a lightweight cycle-only query.
- The fix was to add `has_causal_cycle_without_cache()`, which builds the PORF adjacency and runs Kahn's topo sort but stops before vector-clock computation. The checker uses this when the PORF cache is cold, and reuses `has_causal_cycle()` when the cache is already warm. This way, pruned graphs never build the cache, and consistent graphs build it lazily on the first `porf_contains()` call in `visit()`.
- As a bonus, the graph-building code was refactored to share `build_porf_graph_structure()` and `compute_topological_order()` between the cheap path and `ensure_porf_cache()`. The per-thread event sort was also removed, since `ExplorationGraphT::add_event()` guarantees monotonic per-thread indices and sequential event IDs.
- Phase 3 is now complete. The hybrid follow-up commit (`9f3d012`) brought the same benchmark down to `108.44s`, which is materially better than both the regressed first Phase 3 landing and the Phase 2 baseline.

### Phase 4: Incremental PORF Only In The Safe Narrow Case

Commits: `705def0`, `46ea46a` (Landing 1); `dd60e24` implemented then reverted in `e889693` (Landing 2, skipped); `70ed725` (Landing 1 follow-up on safe-copy acyclicity preservation)

Incremental PORF maintenance is worth doing, but only for the common forward path:

1. append a fresh event
2. optionally assign `rf` for that fresh receive, either to a send or to bottom
3. recurse before any descendants exist

Do not apply this optimization to:

- `with_rf()` on existing receives
- revisit rebinding
- `restrict()`
- arbitrary `set_reads_from()` rewrites on pre-existing structure
- arbitrary `set_reads_from_bottom()` rewrites on pre-existing structure

Expected result:

- lower cost for forward exploration and forward-path cycle checks without weakening correctness in revisit logic

Implementation note:

- After the Phase 3 follow-up, the hot profile still clusters around forward-path cycle/PORF structure work: `has_causal_cycle_without_cache()`, `build_porf_graph_structure()`, `compute_topological_order()`, and `ensure_porf_cache()` remain the dominant DPOR-specific costs.
- That means Phase 4 is still the right next step, but its scope should be read a bit more precisely: incremental maintenance should help both the checker's append-only cycle query and the later `porf_contains()` calls on the same forward branch.
- `unread_send_event_ids()` and `thread_trace()` are still visible, but they are smaller than the cycle/PORF cluster after Phase 3. They do not justify reordering the phases; at most they are candidate follow-up cleanups inside or after Phase 4.

Implementation plan:

#### Landing 1: Known-Acyclic Forward Branches

Goal:

- remove the cold-cache cycle recomputation from the common forward append path
- keep malformed-graph diagnostics and revisit behavior unchanged

Scope:

- add internal acyclicity metadata to `ExplorationGraphT`
- propagate it only through the safe append-only forward path
- let the consistency checker skip the cycle query when that internal invariant is known

State additions in `ExplorationGraphT`:

- `bool known_acyclic_{true};`
- `std::optional<EventId> pending_fresh_receive_id_{};`

This metadata means only that `(po union rf)` is known acyclic. It does not mean the graph is otherwise fully consistent. There should be no public setter such as `mark_known_acyclic()`: the property must stay internal and derived from graph mutations, not asserted by callers.

Mutation rules:

1. `add_event()`
   - if `known_acyclic_` is already true, preserve it
   - clear any existing `pending_fresh_receive_id_`, because the older receive is no longer a fresh leaf
   - if the new event is a receive, set `pending_fresh_receive_id_ = new_id`
   - if the new event is send / non-deterministic choice / block / error, leave the pending id empty
2. `set_reads_from_source()`, `set_reads_from()`, `set_reads_from_bottom()`
   - if `known_acyclic_` is true and the target receive matches `pending_fresh_receive_id_`, preserve `known_acyclic_` and clear the pending id
   - otherwise clear both `known_acyclic_` and `pending_fresh_receive_id_`
3. `with_nd_value()`
   - preserve the acyclicity metadata, because it changes neither `po` nor `rf`
4. conservative operations
   - `with_rf()` and `with_rf_source()` should produce graphs with unknown acyclicity unless the caller uses an explicitly proven-safe rewrite helper
   - `restrict()` and `with_bottom_rf()` should preserve `known_acyclic_` when the source graph is already known acyclic

Rationale for the narrow safe case:

- appending a fresh event to an acyclic graph adds only one immediate `po` edge from that thread's prior leaf; this cannot create a cycle
- assigning `rf` for a freshly appended receive leaf adds at most one incoming `rf` edge into that leaf; because the receive has no `po` successors and no outgoing `rf`, that new edge cannot close a cycle
- none of those arguments hold once an `rf` rewrite targets a pre-existing receive, or once `restrict()` / revisit remapping rewrites the graph shape

Checker integration:

1. keep `validate_graph<false>()` unchanged as the first step in `AsyncConsistencyCheckerT::check(const ExplorationGraphT&)`
2. if `cycle_query_safe == false`, return the collected issues immediately
3. if `graph.is_known_acyclic()`, skip the cycle query entirely
4. otherwise keep the current Phase 3 fallback:
   - warm cache -> `graph.has_causal_cycle()`
   - cold cache -> `graph.has_causal_cycle_without_cache()`

Landing 1 tests:

- forward send append preserves `known_acyclic_`
- forward receive plus `set_reads_from()` preserves `known_acyclic_`
- forward receive plus `set_reads_from_bottom()` preserves `known_acyclic_`
- appending another event before finalizing an older fresh receive clears only the pending id, not the known-acyclic bit
- rewriting `rf` on a pre-existing receive clears known acyclicity
- `restrict()` preserves known acyclicity
- `with_bottom_rf()` preserves known acyclicity
- `with_rf()` clears known acyclicity unless the caller uses the proven-safe helper
- checker parity remains unchanged for malformed graphs and for non-structural issue combinations
- a revisit-derived graph still falls back to ordinary cycle checking

Landing 1 expected perf effect:

- `has_causal_cycle_without_cache()` should largely disappear from the common forward DPOR path
- `build_porf_graph_structure()` and `compute_topological_order()` should become more concentrated in revisit/cold-cache fallback work

Landing 1 measurement result:

- Landing 1 was implemented in commit `705def0`.
- On the same `participants=4`, `iterations=1`, `--no-crash` timeout benchmark used for the earlier Phase 3 measurements, runtime was effectively flat relative to the pre-Landing-1 baseline:
  - `9f3d012` (Phase 3 follow-up baseline): `108440.546 ms`
  - `705def0` (Phase 4 Landing 1): `109047.868 ms`
  - sample duration moved from `108433.552 ms` to `109054.120 ms`
  - execution count stayed unchanged at `7262928`
- The profile shape still changed in the intended direction: `has_causal_cycle_without_cache()` stopped being a top self hotspot and became a smaller residual cost concentrated in conservative fallback/revisit paths.
- The remaining DPOR-specific self hotspots after Landing 1 were still dominated by the PORF/topology cluster and nearby forward-path work:
  - `compute_topological_order()`: `2.80%`
  - `build_porf_graph_structure()`: `2.78%`
  - `validate_graph<false>()`: `2.38%`
  - `thread_trace()`: `2.71%`
  - `backward_revisit()`: `1.81%`
  - `unread_send_event_ids()`: `1.72%`
  - `ensure_porf_cache()`: `1.50%`
  - `porf_contains()`: `0.66%`
- Conclusion: Landing 1 appears structurally correct but not sufficient for end-to-end speedup on this workload. Landing 2 is now skipped in the mainline plan. The prototype notes below remain useful as justification for skipping it, but the next implementation work should move to Phase 5.
- Follow-up: commit `70ed725` widened the meaning of `known_acyclic_` from the original append-only fast-path approximation to "this graph is known acyclic", and preserved that fact across safe graph-copy operations (`restrict()`, `with_bottom_rf()`, and proven-safe revisit rewires via `with_rf_preserving_known_acyclicity()`).

#### Landing 2: Warm-Cache Incremental PORF Extension (Skipped)

Goal:

- avoid full `ensure_porf_cache()` rebuilds when a child graph is produced from a warm, acyclic parent by the same narrow append-only forward path

Scope:

- extend the Landing 1 metadata into a richer append-provenance record only if needed for cache extension
- keep the full rebuild path untouched for all non-forward or structurally unclear cases

Additional state for Landing 2:

- replace or extend `pending_fresh_receive_id_` with append provenance that records:
  - the fresh appended event id
  - its immediate `po` predecessor, if any
  - whether the fresh receive is still awaiting `rf` finalization
  - the finalized fresh `rf` source or bottom, if present
  - a reference-counted snapshot of the parent `PorfCache` only when that parent cache was already warm and acyclic

Fast path in `ensure_porf_cache()`:

1. if a full PORF cache already exists on the current graph, reuse it as today
2. otherwise, if the graph is known acyclic and carries valid append provenance from a warm acyclic parent cache, build the new cache by cloning and extending the parent cache
3. otherwise, fall back to the current full rebuild path

Clone-and-extend algorithm:

1. deep-copy the parent `PorfCache`
2. append `position_in_thread` for the fresh event
3. compute the fresh event's vector clock as:
   - its `po` predecessor's clock, if any
   - pointwise max with the finalized `rf` source clock, if any
   - then set the fresh event's own thread position
4. mark `has_cycle = false`

Important edge case:

- if the fresh event is the first event on a previously unseen thread, the cache width increases by one and all existing clocks must be widened consistently before writing the new event's clock

Conservative fallback remains mandatory for:

- `restrict()`
- `with_rf()` / revisit rebinding
- arbitrary `set_reads_from()` rewrites on pre-existing receives
- arbitrary `set_reads_from_bottom()` rewrites on pre-existing receives
- any graph whose parent cache was cold or whose append provenance is incomplete

Landing 2 tests:

- a warm cached parent plus one fresh append produces the same `porf_contains()` answers as a full rebuild
- a warm cached parent plus fresh receive-and-`rf` append produces the same `porf_contains()` answers as a full rebuild
- the first-event-on-a-new-thread widening case matches full rebuild results
- unsafe rewrites still invalidate the fast path and fall back to the conservative rebuild

Landing 2 expected perf effect:

- `ensure_porf_cache()` should shift from full rebuild work toward clone-and-extend work on forward branches
- `build_porf_graph_structure()` and `compute_topological_order()` should become mostly fallback/revisit costs
- allocator-heavy PORF rebuild work should drop again if the new clone cost is lower than the rebuild it replaces

Landing 2 measurement result:

- Landing 2 was implemented in commit `dd60e24` and then reverted after measurement.
- On the same `participants=4`, `iterations=1`, `--no-crash` timeout benchmark used for the earlier measurements, runtime improved only slightly relative to Landing 1:
  - `705def0` (Phase 4 Landing 1): `109047.868 ms`
  - `dd60e24` (Phase 4 Landing 2): `108529.249 ms`
  - sample duration moved from `109054.120 ms` to `108605.343 ms`
  - execution count stayed unchanged at `7262928`
- The new `perf` trace remained dominated by allocator work (`_int_malloc`, `_int_free`, `malloc`, `malloc_consolidate`), which means clone-and-extend is still paying meaningful deep-copy cost even when it avoids a full PORF rebuild.
- The remaining DPOR-specific self hotspots after Landing 2 were still:
  - `compute_topological_order()`: `2.78%`
  - `thread_trace()`: `2.69%`
  - `build_porf_graph_structure()`: `2.66%`
  - `validate_graph<false>()`: `2.29%`
  - `backward_revisit()`: `1.86%`
  - `unread_send_event_ids()`: `1.80%`
  - `build_full_porf_cache()`: `1.42%`
  - `porf_contains()`: `0.71%`
- The incremental helper itself was effectively invisible as a self hotspot, which suggests either:
  - the warm-parent append-only fast path is not hit often enough in this benchmark
  - or the deep copy of the parent cache largely cancels out the rebuild it replaces
- Conclusion: the Landing 2 idea was valid enough to prototype, but not valuable enough to keep. The current tree keeps Landing 1 only. The next priority should move to graph-materialization / allocator reduction on the forward path, with revisit-heavy materialization still visible as a secondary concern.

Phase 4 closeout:

1. keep Landing 1 in tree
2. skip Landing 2 unless a later profile shows full PORF rebuilds becoming dominant again
3. use the Landing 1 and reverted-Landing-2 measurements as the handoff rationale for Phase 5

### Phase 5: Eliminate Common-Case Branch Copies In One Worker

Commits: `59eb233`

After the Phase 4 measurements, this is now the most plausible next major step. The current profile is still allocator/materialization-heavy, and the remaining whole-graph copies in `visit()` are now a cleaner target than more PORF-local work.

Phase 5 scope:

- remove full-graph value copies from the common forward branches in `visit()`
- keep all mutation worker-local to one recursion stack
- preserve the current revisit logic (`restrict()`, `with_rf()`, `revisit_condition()`, `backward_revisit()`) except for minimal signature adjustments needed to call the new recursion helper
- preserve the current by-value ownership model at the task-spawn boundary for any future parallel search

Phase 5 non-goals:

- do not redesign revisit materialization yet; that remains Phase 6
- do not try to preserve or incrementally extend `PorfCache` across rollback in the first implementation
- do not weaken the rule that every graph recursively explored by `visit()` must already be fully consistent
- do not introduce shared mutable graph state across workers

Implementation plan:

#### Landing 1: Worker-Local Checkpoint / Rollback Infrastructure

Goal:

- make `ExplorationGraphT` cheap to mutate temporarily and then restore exactly

API shape:

- add a small worker-local `Checkpoint` token to `ExplorationGraphT`
- add `checkpoint()` and `rollback(Checkpoint)` on `ExplorationGraphT`
- optionally add a tiny RAII helper (`ScopedRollback` or equivalent) so `visit()` cannot accidentally miss a rollback on an early return
- the token should just capture the current rollback frontier (for example, undo-log sizes or equivalent logical mutation counts); no separate shared stack of checkpoints is required

Implementation shape:

1. keep rollback state inside `ExplorationGraphT`; it is worker-local bookkeeping, not part of the graph's logical value
2. make `Checkpoint` token-based rather than stack-based:
   - each recursive call frame takes one checkpoint before mutating
   - `rollback(token)` undoes mutations back to that token's frontier
   - recursion gives the needed LIFO behavior naturally, without a global checkpoint stack API
3. add an event-append undo log that records enough state to undo one `add_event()`:
   - appended event id
   - thread id
   - appended per-thread event index
   - previous `ThreadState`
   - previous `next_event_index_by_thread_` entry for that thread
4. add an `rf`-assignment undo log that records enough state to undo one `set_reads_from*()`:
   - receive id
   - whether the receive previously had an `rf` entry
   - previous `ReadsFromSource`, if any
5. extend `ExecutionGraphT` only with the minimal internal hooks needed to support rollback:
   - pop the last appended event
   - erase a used event index from the owning thread's set
   - restore `next_event_index_by_thread_`
   - clear or restore a `reads_from_` entry
   - note explicitly that the `used_event_indices_by_thread_` update is the one rollback step that is not a pure vector truncation/restore; it is an `unordered_set<EventIndex>::erase()` and should be implemented and measured as such
6. let rollback restore logical contents, not storage capacity:
   - vectors and dense `reads_from_` storage may keep reserved capacity after rollback
   - the important invariant is exact logical state restoration, not shrinking memory on every undo
7. always clear `porf_cache_` on mutation and on rollback in the first implementation
8. checkpoint/rollback must also save and restore the Phase 4 metadata:
   - `known_acyclic_`
   - `pending_fresh_receive_id_`
9. copy-producing operations (`restrict()`, `with_rf()`, `with_bottom_rf()`, ordinary graph copy/snapshot paths) must not retain worker-local undo history from the source graph; copied graphs should start with empty rollback history even if they preserve the same logical execution state
10. this clean-copy rule matters especially for the send branch after Landing 2:
   - `backward_revisit()` must inspect the currently mutated parent graph, so the fresh send is visible to `porf_contains()` and restriction logic
   - any `restrict()` / `with_rf()` graphs materialized inside `backward_revisit()` must still be independent values with empty rollback history

Landing 1 tests:

- append one event, then roll back to the empty graph
- append a receive, assign `rf`, then roll back and confirm both the event and its `rf` entry disappear
- overwrite an existing `rf` entry, then roll back and confirm the previous source is restored
- token-based checkpoints compose correctly across nested recursive use; one explicit nested-checkpoint unit test is enough, but the implementation does not need a separate checkpoint stack
- `thread_event_count()`, `last_event_id()`, `thread_trace()`, insertion order, and `inserted_before_or_equal()` all match the pre-mutation state after rollback
- `known_acyclic_` and `pending_fresh_receive_id_` are restored correctly across rollback
- checkpoint -> append receive -> assign `rf` -> rollback -> append a different receive -> assign a different `rf` source still preserves correct `known_acyclic_` tracking on the restored graph
- warming `porf_cache_`, mutating, and rolling back remains correct even though the cache is conservatively dropped
- copying a graph after Landing 1 produces an independent logical graph with no borrowed worker-local rollback history
- `restrict()` and `with_rf()` also produce independent graphs with no borrowed worker-local rollback history when called from a graph that already has rollback history

#### Landing 2: Rewrite Forward Recursion To Mutate, Recurse, Roll Back

Goal:

- remove `auto new_graph = graph` from the common forward branches without touching revisit-heavy materialization yet

Recursive shape:

1. keep `verify()` as the spawn-facing, by-value entry point
2. add an internal worker-local recursive helper shape where:
   - `visit(...)` takes `ExplorationGraphT&`
   - `visit_if_consistent(...)` takes `ExplorationGraphT&`
3. keep by-reference recursion strictly inside one worker; any future task split must still hand an owned graph value to the other worker

Branch rewrite:

1. error branch
   - checkpoint
   - append the terminal error event
   - publish the execution / error result
   - if `config.on_execution` is set, call it before rollback so the observer sees the terminal error event on a fully materialized branch-local graph
   - roll back before returning to the caller's branch
2. ND branch
   - one checkpoint per choice
   - append the chosen ND event
   - recurse
   - roll back
3. receive branch
   - collect compatible unread sends before mutating
   - for each compatible send: checkpoint, append receive, set `rf`, call `visit_if_consistent()`, roll back
   - for non-blocking receives: do the same for the bottom branch
4. send branch
   - checkpoint
   - append the send once
   - run `backward_revisit()` against the currently mutated graph, so the fresh send is visible to revisit checks and any cache warming via `porf_contains()`
   - recurse forward on that same mutated graph
   - roll back after both the revisit work and the forward continuation finish
5. block branch
   - checkpoint
   - append the internal `Block` event
   - recurse
   - roll back

Observer ordering rule:

- at every `config.on_execution(graph)` callback, the observer must see a complete branch-local graph state for that execution
- error branch: callback happens after the error event is appended and before rollback
- normal completion branch: callback happens at the base case on the current fully materialized graph; caller checkpoints are rolled back only after that recursive call returns
- no callback should ever observe a partially rolled-back graph

Intentional hold-outs for Phase 6:

- `backward_revisit()` may continue to materialize via `restrict()` plus `with_rf()`
- `revisit_condition()` may continue to build restricted graphs
- `reschedule_blocked_receive_if_enabled()` may continue to use `restrict()` to drop the block event

Landing 2 tests:

- existing DPOR end-to-end tests and oracle-backed tests remain unchanged
- add one targeted regression that mixes ND, send, blocking receive, non-blocking receive, and error/block paths so execution sets are compared before and after the refactor
- confirm observer callbacks always see fully materialized branch-local graphs:
  - terminal error execution before rollback
  - ordinary completed execution before the caller unwinds and rolls back its checkpoint
- confirm the send branch still exposes the appended send to `backward_revisit()`, while the caller-visible graph after rollback does not retain it
- confirm `restrict()` / `with_rf()` materialized during `backward_revisit()` remain independent graphs with clean rollback state even when the parent graph is currently mutated and has rollback history

Phase 5 measurement plan:

1. land Landing 1 with rollback-focused unit tests
2. land Landing 2 and rerun focused DPOR tests, then `ctest --preset debug --output-on-failure`
3. rerun the timeout benchmark and `perf`
4. confirm that whole-graph copy construction / destruction largely disappears from the common forward `visit()` path
5. only then decide whether Phase 6 revisit-specific work is still justified

Expected result:

- largest remaining reduction in ordinary branch-state materialization
- lower allocator pressure on the forward path
- remaining graph materialization should become more concentrated in revisit-specific helpers, making Phase 6 a cleaner follow-up target

Phase 5 measurement result:

- Phase 5 was implemented in commit `59eb233`.
- On the same `participants=4`, `iterations=1`, `--no-crash` timeout benchmark used throughout this plan, the release benchmark showed a clear improvement relative to the pre-Phase-5 parent commit:
  - `fdfc4bb` (pre-Phase-5 baseline): `105347.809 ms`
  - `59eb233` (Phase 5): `91023.412 ms`
  - sample wall time moved from `105.34 s` to `91.02 s`
  - execution count stayed unchanged at `7262928`
  - net improvement: `14324.397 ms` (`13.6%`, about `1.16x` faster)
- A fresh post-Phase-5 `perf` capture was then recorded with `benchmarks/profile_two_phase_commit_timeout_perf.sh --participants 4` on commit `59eb233`. Under that `RelWithDebInfo` profiling build, the benchmark run was `92887.396 ms` and `perf` captured `46309` samples.
- Allocator work still dominates the profile overall:
  - `_int_free`: `10.62%`
  - `_int_malloc`: `8.88%`
  - `malloc`: `8.02%`
  - `malloc_consolidate`: `3.28%`
- The remaining DPOR-specific self hotspots after Phase 5 were:
  - `compute_topological_order()`: `3.48%`
  - `thread_trace()`: `3.09%`
  - `build_porf_graph_structure()`: `2.73%`
  - `validate_graph<false>()`: `2.56%`
  - `backward_revisit()`: `2.26%`
  - `unread_send_event_ids()`: `2.09%`
  - `add_event()`: `1.96%`
  - `ensure_porf_cache()`: `1.67%`
  - `restrict()`: `0.88%`
  - `porf_contains()`: `0.72%`
- The profile shape is the important follow-up signal:
  - the old common forward-path whole-graph copy construction / destruction is no longer visible as a dominant standalone self hotspot
  - the remaining allocation-heavy stacks now point mostly through revisit-local materialization such as `backward_revisit()`, `restrict()`, and `with_rf()`
  - PORF/cycle work is still visible, but it is now increasingly tied to revisit-heavy paths rather than the forward branch-copy shape that Phase 5 targeted
- Conclusion: Phase 5 delivered the expected end-to-end win and made the remaining graph-materialization cost more revisit-local. That means Phase 6 is still worth doing, and the post-Phase-5 trace strengthens that case rather than weakening it.

Phase 5 newly exposed hotspots:

The forward-path copy elimination uncovered costs that were previously hidden behind the larger copy overhead:

- `vector<unsigned long>::_M_realloc_insert`: `4.30%` self — `push_back` on the `successors` adjacency list (`vector<vector<EventId>>`) inside `build_porf_graph_structure()` triggering repeated reallocation. This is rebuilt from scratch on every `has_causal_cycle_without_cache()` and `ensure_porf_cache()` call.
- `unordered_map<EventId, pair<EventId const, EventId>>::operator[]`: `3.87%` self — hash-map lookups on `EventId → EventId` maps. There are four such maps in the codebase:
  - `exploration_graph.hpp`: `rf_source` and `po_pred` inside `ensure_porf_cache()` (vector-clock computation)
  - `exploration_graph.hpp`: `id_map` inside `restrict()` (old→new event ID remapping)
  - `dpor.hpp`: `id_map` inside `revisit_condition()` (same remapping pattern)
- `unordered_set<EventId>` insert/rehash: `1.37%` + `1.06%` + `0.77%` combined — the `keep_set`, `deleted`, and `previous` sets in `backward_revisit()` and `revisit_condition()`.
- `add_event()`: `1.96%` self — undo-log bookkeeping on every forward-path append. This is new Phase 5 overhead, acceptable relative to the copy cost it replaced.
- `vector<unordered_set<EventIndex>>::vector(copy)`: `0.54%` self — `used_event_indices_by_thread_` deep-copied during `restrict()` / `with_rf()` graph materialization.
- `variant` move/copy construct: ~`1.4%` combined — `EventLabelT` variant construction during event appends and graph copies.

These newly visible costs are the natural targets for Phase 5.5 (low-risk representation cleanups) and Phase 6 (revisit-specific materialization).

### Phase 5.5: Internal Dense Remap/Mask Helpers + PORF Successor Pre-Sizing

Commits: `ca5c2b9`, `c978d9e`

The Phase 5 profile exposed `unordered_map<EventId, EventId>`, `unordered_set<EventId>`, and PORF successor-list reallocation as newly prominent hotspots. These are the same kind of hash-based containers that Phase 2 successfully replaced with dense vectors for event-indexed and thread-indexed state. The same treatment applies here, plus pre-sizing for the adjacency vectors that are now the single largest non-allocator self hotspot.

Phase 5.5 scope:

- replace the remaining `EventId`-keyed hash maps and hash sets with dense vectors in internal helpers
- pre-size PORF successor adjacency lists to eliminate repeated reallocation
- keep all changes internal: do not change public API signatures on `restrict()`, `with_rf()`, or any other `ExplorationGraphT` surface
- do not change algorithm semantics

Phase 5.5 non-goals:

- do not redesign `restrict()`, `backward_revisit()`, or `revisit_condition()` logic or ownership model
- do not change the graph materialization ownership model
- do not touch forward-path rollback infrastructure
- do not couple internal remap helpers to the public `restrict()` contract

Implementation note on dense masks: prefer `std::vector<std::uint8_t>` over `std::vector<bool>` for event-indexed masks. `vector<bool>`'s proxy specialization introduces indirection and bit-packing that hurts performance in hot loops. A plain byte vector indexed by `EventId` is simpler and faster.

#### Landing 1: PORF Successor Pre-Sizing And Dense Helpers In Cache Construction

Targets:
- `add_po_edges()` at `exploration_graph.hpp` — pushes into `successors[pred]` without pre-sizing
- `add_rf_edges()` at `exploration_graph.hpp` — pushes into `successors[source_id]` without pre-sizing
- `ensure_porf_cache()` at `exploration_graph.hpp` — `rf_source` and `po_pred` unordered_maps

The `vector<unsigned long>::_M_realloc_insert` hotspot at `4.30%` self comes from `successors[x].push_back(y)` in `add_po_edges()` and `add_rf_edges()`. Each per-event successor vector starts empty and grows by reallocation. Pre-compute the out-degree of each event (one pass over PO structure + one pass over RF relation), then `reserve()` each successor vector before inserting edges. This avoids repeated reallocation without changing the adjacency-building logic.

Also replace the two unordered_maps in `ensure_porf_cache()` (the vector-clock computation path):
- `rf_source`: replace with `std::vector<EventId>` of size `event_count()`, sentinel `kNoSource` for events without an RF source
- `po_pred`: replace with `std::vector<EventId>` of size `event_count()`, sentinel `kNoSource` for events without a PO predecessor

Landing 1 tests:
- existing PORF cache tests and cycle detection tests remain unchanged
- `porf_contains()` results match before and after

#### Landing 2: Internal Dense ID Remap In `restrict()` And `revisit_condition()`

Targets:
- `restrict()` at `exploration_graph.hpp` — internal `id_map` unordered_map
- `revisit_condition()` at `dpor.hpp` — internal `id_map` unordered_map (same pattern)

Both build an `unordered_map<EventId, EventId>` to map old event IDs to new IDs in a restricted graph. Both are bounded by `event_count()`.

Replace each with a local `std::vector<EventId>` of size `event_count()`, initialized to `kNoSource`, indexed by old event ID. This is a purely internal change: `restrict()` keeps its current `const unordered_set<EventId>&` parameter signature. `revisit_condition()` keeps its current signature.

The `revisit_condition()` remap duplicates work that `restrict()` already does internally. A private shared helper that builds the dense old→new remap from a keep set could serve both call sites, but only introduce that factoring if it falls out naturally. Do not change `restrict()`'s return type or add output parameters to expose the remap externally.

Landing 2 tests:
- existing `restrict()`, revisit condition, and DPOR end-to-end tests remain unchanged

#### Landing 3: Dense Masks In `backward_revisit()` And `compute_previous_set()`

Targets:
- `backward_revisit()` at `dpor.hpp` — `deleted` and `keep_set` unordered_sets
- `compute_previous_set()` at `dpor.hpp` — returns `unordered_set<EventId>`

Replace:
- `deleted`: `std::vector<std::uint8_t>` of size `event_count()`, where `1` means deleted
- `keep_set`: derive from the complement of `deleted` without building a second container; pass the dense mask to a private `restrict()` overload or convert to the public `unordered_set` at the call boundary if needed
- `compute_previous_set()`: return `std::vector<std::uint8_t>` instead of `unordered_set<EventId>`, since its only caller (`revisit_condition()`) can use the dense mask directly

If converting the dense mask to `unordered_set` at the `restrict()` call boundary is too wasteful, add a private `restrict_impl(const std::vector<std::uint8_t>&)` overload that `restrict(const unordered_set<...>&)` delegates to. This keeps the public API stable while letting internal callers avoid the hash-set round trip.

Landing 3 tests:
- existing backward revisit, revisit condition, and DPOR end-to-end tests remain unchanged
- existing oracle agreement tests remain unchanged

Phase 5.5 expected result:

- the `4.30%` `vector::_M_realloc_insert` hotspot should largely disappear from PORF adjacency construction
- the `3.87%` `unordered_map::operator[]` hotspot should largely disappear
- the `1.37%` + `1.06%` + `0.77%` hash-set insert/rehash costs should drop
- `restrict()` and `backward_revisit()` should become cheaper per call
- total allocator share should decrease further

Phase 5.5 measurement plan:

1. land each change and run `ctest --preset debug --output-on-failure`
2. after all landings, rerun the timeout benchmark and `perf`
3. confirm execution count unchanged, then compare `elapsed_ms` and hotspot shapes

Phase 5.5 measurement result:

- On the same `participants=4`, `iterations=1`, `--no-crash` timeout benchmark used throughout this plan, the Phase 5.5 implementation improved further relative to the Phase 5 baseline:
  - `59eb233` (Phase 5 baseline): `91023.412 ms`
  - Phase 5.5 implementation: `79002.909 ms`
  - execution count stayed unchanged at `7262928`
  - net improvement versus Phase 5: `12020.503 ms` (`13.2%`, about `1.15x` faster)
- A fresh post-Phase-5.5 `perf` capture was recorded with `benchmarks/profile_two_phase_commit_timeout_perf.sh --participants 4`. Under that `RelWithDebInfo` profiling build, the benchmark run was `79992.154 ms` and `perf` captured `39889` samples.
- The targeted easy hotspots moved the way this phase intended:
  - the old `vector::_M_realloc_insert` hotspot from PORF successor growth is no longer a leading self hotspot
  - the old `unordered_map<EventId, EventId>::operator[]` hotspot is no longer a leading self hotspot
  - `restrict()` / revisit scratch state no longer spends visible top-self time in `unordered_set<EventId>` construction
- The remaining DPOR-specific self hotspots after Phase 5.5 were more concentrated in the next layer down:
  - `ExecutionGraphT::add_event_with_index(...)`: `1.48%`
  - `ExplorationGraphT::add_rf_edges(...)`: `1.41%`
  - `ExplorationGraphT::porf_contains(...)`: `0.98%`
  - `ExplorationGraphT::restrict_from_keep_mask(...)`: `0.85%`
  - `compute_next_event(...)`: `0.84%`
  - `ExplorationGraphT::set_reads_from_source(...)`: `0.82%`
- The remaining profile shape is the important follow-up signal:
  - revisit-local materialization is now clearer as the next target
  - `with_rf()` / `ExecutionGraphT` copy costs, including `used_event_indices_by_thread_` copying, are still visible
  - allocator-heavy destructor/copy stacks now dominate more than the earlier obvious hash-map / reallocation artifacts
- Conclusion: Phase 5.5 delivered the intended low-risk cleanup win and removed the newly exposed obvious hotspots. That strengthens the case for Phase 6 rather than weakening it: the next meaningful work should target revisit-specific graph materialization and copy costs.

### Phase 6: Tackle Revisit-Specific Materialization

Commits: `7f98c8f` (Landing 1); `06ce76b` (Landing 2)

Only after the forward path is cheaper and the representation cleanups are done should revisit-heavy structure be attacked:

1. reduce temporary `with_rf()` copies
2. introduce a restricted view for revisit checks if needed
3. keep compact graph materialization only where the algorithm genuinely needs independent restricted state

This phase is intentionally later because it is more complex and easier to get wrong.

Current direction:

- The most obvious remaining revisit-local waste is in `backward_revisit()`: it materializes `restricted = restrict_masked(...)` and then immediately pays a second whole-graph copy via `with_rf_preserving_known_acyclicity(...)`.
- The receive branch of `revisit_condition()` still materializes `G|Previous` only to compute `get_cons_tiebreaker(G|Previous, e)`. That remains a good second target, but it is a broader semantic change than the extra `with_rf()` copy removal.
- Therefore Phase 6 should start with the narrower backward-revisit copy elimination, and only then move to a masked/view-style tiebreaker path if perf still justifies it.

#### Landing 1: Eliminate The Extra `with_rf()` Copy In `backward_revisit()`

Goal:

- keep the existing `restrict()`-style materialization for revisit children
- remove the immediately-following full-graph copy that only rewires one receive's `rf`

Implementation shape:

1. add a dedicated in-place helper on an already-owned `ExplorationGraphT` child for rebinding one receive's `rf`
2. preserve the existing acyclicity proof structure:
   - `backward_revisit()` already proves the rewire safe via `!porf_contains(recv, send)`
   - the child should keep `known_acyclic_` when this helper is used on the restricted graph
3. keep the worker-local rollback rule intact:
   - do not reuse plain `set_reads_from()` on the materialized child and leave its undo history behind
   - the emitted revisit child must still have clean worker-local history, just like `restrict()` / `with_rf()` copies today
4. leave `revisit_condition()` and blocked-receive rescheduling unchanged in this landing

Why this should come first:

- it is the narrowest remaining copy on the hot revisit path
- it does not require changing the semantics of `G|Previous`
- it should directly reduce the visible `with_rf()` / `ExecutionGraphT` copy cost that remained after Phase 5.5

Landing 1 tests:

- backward revisit still emits the same executions and preserves execution counts
- emitted revisit children still have clean rollback history and can be explored independently
- safe rewires performed by the new in-place helper preserve `known_acyclic_`
- existing backward-revisit and consistency tests remain unchanged

#### Landing 2: Compute Receive `revisit_condition()` Without Materializing `G|Previous`

Goal:

- remove owned `restrict_masked()` materialization from the receive-specific branch of `revisit_condition()`

Implementation shape:

1. keep `compute_previous_set()` as the source of the keep mask unless a better dense formulation naturally falls out
2. replace the current "build restricted graph, remap ids, run `get_cons_tiebreaker()`" flow with a private masked helper that computes the same tiebreaker directly on the original graph plus the keep mask
3. preserve the exact Must semantics:
   - the tiebreaker must still be computed on `G|Previous`, not on the full graph
   - deleted intermediate events must be skipped so the effective program-order predecessor inside the masked computation matches the restricted graph, not the original graph
4. keep the non-receive and non-blocking receive branches unchanged unless the masked helper naturally subsumes them

Why this is second:

- it removes a larger revisit-local materialization cost, but it is easier to get wrong than Landing 1
- the correctness burden is semantic, not just ownership-related: the masked helper must behave exactly like restricting the graph first

Landing 2 tests:

- existing `get_cons_tiebreaker()` and `revisit_condition()` regressions still pass unchanged
- targeted regression where `Previous` omits intermediate same-thread events, proving the masked computation skips deleted PO predecessors correctly
- targeted regression where the current `rf(e)` lies outside `Previous`, still returning false exactly as today

#### Landing 3: Perf-Gated Follow-Ups Only If Needed

If revisit-local materialization still dominates after Landings 1 and 2, then consider one of these narrower follow-ups:

1. a one-pass restricted-child materializer that combines `restrict()` plus the final `rf` rebind in one build
2. removing the `restrict_masked()` materialization in blocked-receive rescheduling
3. a broader restricted-view design only if the smaller owned-child reductions are not enough

Measurement rule for Phase 6:

1. land Landing 1 and rerun focused DPOR tests plus `ctest --preset debug --output-on-failure`
2. re-measure before starting Landing 2
3. only continue to the masked/view-style work if the profile still points at revisit-specific materialization

Phase 6 measurement result:

- Landing 1 was implemented in commit `7f98c8f`.
- Landing 2 was implemented in commit `06ce76b`.
- On the same `participants=4`, `iterations=1`, `--no-crash` timeout benchmark used throughout this plan, Landing 2 produced a meaningful additional end-to-end win relative to the Landing 1 tree:
  - `7f98c8f` (Phase 6 Landing 1): `67268.974 ms`
  - `06ce76b` (Phase 6 Landing 2): `58006.204 ms`
  - execution count stayed unchanged at `7262928`
  - net improvement versus Landing 1: `9262.770 ms` (`13.8%`, about `1.16x` faster)
- The preferred `perf` workflow could not be used in this environment because `perf` was unavailable, so a separate `RelWithDebInfo -pg` build was profiled with `gprof` on the same benchmark workload. Under that profiling build, the benchmark run was `105708.222 ms` with the same `7262928` executions.
- The important result from that post-Landing-2 profile is that the targeted revisit-condition materialization no longer appears as a meaningful hotspot:
  - `revisit_condition(...)`: `0.28%`
  - `MaskedPorfContext(...)`: `0.19%`
  - `compute_previous_set(...)`: `0.14%`
  - `get_cons_tiebreaker_masked(...)`: `0.09%`
- The larger remaining DPOR-specific self hotspots after Phase 6 shifted elsewhere:
  - `validate_graph<false>()`: `8.67%`
  - `thread_trace()`: `7.64%`
  - `unread_send_event_ids()`: `7.50%`
  - `build_porf_graph_structure()`: `5.63%`
  - `for_each_backward_revisit_child(...)`: `4.74%`
  - `porf_contains()`: `2.39%`
  - `restrict_from_keep_mask()`: `2.34%`
  - `add_event_with_index()`: `2.30%`
  - `set_reads_from_source()`: `1.64%`
- Conclusion: Phase 6 succeeded and should stop at Landing 2. Landing 3 no longer looks like the best next use of effort. If revisit-local work is revisited later, the remaining candidate is `restrict_from_keep_mask()` / blocked-receive materialization; but based on the current profile shape, the next priority should move to incremental derived-state work around `thread_trace()` and unread-send tracking rather than more `revisit_condition()`-specific optimization.

Phase 6 closeout:

1. keep Landing 1 and Landing 2 in tree
2. defer Landing 3 unless a fresh profile makes `restrict()` / blocked-receive materialization dominant again
3. move the next performance phase toward `thread_trace()`, unread-send tracking, and related repeatedly recomputed derived state

## Suggested Landing Order

1. example-local structured `ValueT`
2. dense storage for clearly dense event-ID-indexed fields
3. trivial extra-copy and missed-move fixes in `visit()`
4. unify cycle work without changing diagnostics
5. re-measure and confirm whether the dominant remaining costs are cycle/PORF, unread-send scanning, or graph materialization
6. Phase 4 Landing 1: internal known-acyclic propagation for the append-only forward path, so the checker can skip cold cycle queries there
7. skip Phase 4 Landing 2 in the mainline plan; keep the reverted prototype only as a data point unless later profiling changes the tradeoff
8. Phase 5 Landing 1: worker-local checkpoint/rollback infrastructure, including rollback-focused tests and empty-history copy semantics for materialized graphs
9. Phase 5 Landing 2: reference-based forward recursion in `visit()`, while leaving revisit materialization intentionally unchanged
10. Phase 5.5: densify remaining hash maps and hash sets in PORF cache construction, `restrict()`, `revisit_condition()`, and `backward_revisit()`
11. Phase 6: completed with the backward-revisit `with_rf()` copy removal and the masked `G|Previous` tiebreaker path; do not prioritize Landing 3 without a fresh profile
12. reserve hot vectors and other minor adjacency-building cleanups where perf justifies them

## Why This Order

This order balances:

- immediate wins
- implementation safety
- compatibility with future parallel search
- preservation of current correctness and diagnostics

It also avoids paying the complexity cost of a rollback-based redesign before the cheaper and more local wins have been measured.

After Phase 2, the event-ID cleanup removed a large chunk of graph-copy overhead, so Phases 5 and 6 needed to stay perf-gated. After Phase 4, that gating pointed toward Phase 5: Phase 4 fixed the forward-path cycle/PORF structure cleanly, but the remaining profile was still too allocator-heavy for more PORF-local work. Phase 5 delivered the forward-path copy elimination. Phase 5.5 cleaned up the newly exposed hash-map and reallocation hotspots. Phase 6 then removed the obvious revisit-only extra materialization costs and paid off. The remaining profile shape no longer points first at `revisit_condition()` materialization; it now points more strongly at repeatedly recomputed derived state such as `thread_trace()`, unread-send discovery, and the remaining PORF / restriction work.

## Acceptance Criteria

Each phase should preserve:

- execution counts
- oracle agreement
- full-consistency guarantees for explored graphs
- no mutable-state sharing across parallel task boundaries

Each completed phase should also show at least one measurable improvement in either:

- end-to-end runtime
- allocator-heavy symbol share
- cycle-checking cost
- graph-copy visibility in perf

## How To Run Performance Comparisons

This section describes the three measurement workflows used to evaluate each phase: end-to-end timing, `perf` profile recording, and profile analysis.

### Prerequisites

- Linux with `perf` installed (`linux-tools-common` / `linux-tools-$(uname -r)`)
- Ninja build system
- A C++20 compiler (GCC 12+ or Clang 15+)
- `perf_event_paranoid` set to allow user-space recording (`sudo sysctl -w kernel.perf_event_paranoid=1` or lower)

### 1. End-To-End Timing

Build a clean release benchmark binary and run it with the standard workload. The reference workload for cross-phase comparisons is `participants=4`, `iterations=1`, `--no-crash` on the timeout-inclusive 2PC model.

```bash
# Build (release, no test harness overhead)
cmake -S . -B build/bench-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DDPOR_BUILD_TESTING=OFF \
  -DDPOR_BUILD_EXAMPLES=ON \
  -DDPOR_BUILD_BENCHMARKS=ON

cmake --build build/bench-release \
  --target dpor_two_phase_commit_timeout_benchmark -j

# Run the reference workload
build/bench-release/benchmarks/two_phase_commit_timeout/dpor_two_phase_commit_timeout_benchmark \
  --mode dpor --participants 4 --iterations 1 --no-crash
```

Output looks like:

```
2PC timeout benchmark participants=4 communication_model=async inject_crash=false iterations=1 progress_interval_ms=1000 optimized_build=true
DPOR
  progress run 1: elapsed_ms=1000.000 terminal_executions=64021 full_executions=64021 error_executions=0 depth_limit_executions=0 active_workers=1/1 queued_tasks=0/0 counts_exact=true
  run 1: terminal_executions=7262928 full_executions=7262928 error_executions=0 depth_limit_executions=0 elapsed_ms=108440.546
  summary: min_ms=108440.546 avg_ms=108440.546 max_ms=108440.546 terminal_executions=7262928 full_executions=7262928 error_executions=0 depth_limit_executions=0
```

Key values to record for each phase:
- `elapsed_ms` (the `summary: min_ms` value when using `--iterations 1`)
- `terminal_executions`, `full_executions`, `error_executions`, and `depth_limit_executions` (these must stay constant across phases — a change means a correctness regression)

For more stable numbers, use `--iterations 3` and compare `min_ms`.

### 2. Perf Profile Recording

Use the provided script, which builds a `RelWithDebInfo` binary with frame pointers and records a `cpu-clock` trace with DWARF unwinding:

```bash
benchmarks/profile_two_phase_commit_timeout_perf.sh --participants 4
```

This does three things:
1. Configures and builds `build/perf/benchmarks/two_phase_commit_timeout/dpor_two_phase_commit_timeout_benchmark` with `-O2 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls`
2. Runs `perf record -e cpu-clock --all-user -F 499 --call-graph dwarf,4096` on the benchmark with `--mode dpor --participants 4 --iterations 1 --no-crash`
3. Writes the trace to `benchmarks/perf-data/perf-two-phase-commit-timeout-p4-<commit>-<timestamp>.data`

The script automatically tags the output file with the current git short-hash and a timestamp, so you can record before and after a change and compare.

If you need to profile a different configuration manually:

```bash
cmake -S . -B build/perf -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDPOR_BUILD_TESTING=ON \
  -DDPOR_BUILD_EXAMPLES=ON \
  -DDPOR_BUILD_BENCHMARKS=ON \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls"

cmake --build build/perf \
  --target dpor_two_phase_commit_timeout_benchmark -j

perf record -e cpu-clock --all-user -F 499 \
  --call-graph dwarf,4096 \
  -o my-trace.data -- \
  build/perf/benchmarks/two_phase_commit_timeout/dpor_two_phase_commit_timeout_benchmark \
  --mode dpor --participants 4 --iterations 1 --no-crash
```

If stacks look truncated, increase the kernel callchain depth:

```bash
sudo sysctl -w kernel.perf_event_max_stack=512
```

### 3. Profile Analysis

#### Self-cost hotspots (flat profile)

Shows which functions spend the most time in their own code (excluding callees). This is the primary view for identifying which symbols to optimize:

```bash
perf report -i <trace>.data --stdio --no-children --percent-limit 0.5
```

Key DPOR-specific symbols to watch across phases:

| Symbol | What it means |
|---|---|
| `build_porf_graph_structure()` | PORF adjacency construction |
| `compute_topological_order()` | Kahn's topo sort for PORF |
| `build_full_porf_cache()` | vector-clock computation |
| `ensure_porf_cache()` | lazy PORF cache entry point |
| `has_causal_cycle_without_cache()` | cold-cache cycle check |
| `validate_graph<false>()` | RF endpoint validation |
| `thread_trace()` | per-thread value extraction |
| `unread_send_event_ids()` | compatible-send scan |
| `backward_revisit()` | revisit logic |
| `porf_contains()` | PORF reachability query |

Allocator symbols (`_int_malloc`, `_int_free`, `malloc`, `malloc_consolidate`, `operator new`, `operator delete`) indicate graph-copy / materialization overhead.

#### Caller-oriented view (cumulative profile)

Shows cumulative cost including callees, useful for understanding which high-level code paths dominate:

```bash
perf report -i <trace>.data --stdio --children -g graph,0.5,caller
```

#### Comparing two traces

To compare hotspot shapes before and after a change, run the flat profile on both traces and diff the DPOR-specific symbols:

```bash
# Before
perf report -i benchmarks/perf-data/perf-...-<before-commit>-....data \
  --stdio --no-children --percent-limit 0.3 > /tmp/before.txt

# After
perf report -i benchmarks/perf-data/perf-...-<after-commit>-....data \
  --stdio --no-children --percent-limit 0.3 > /tmp/after.txt

diff /tmp/before.txt /tmp/after.txt
```

The important things to check in a comparison:
1. **End-to-end `sample duration`** at the top of the report (corresponds to wall-clock time under profiling)
2. **DPOR-specific symbol percentages** shifting in the expected direction (e.g., `has_causal_cycle_without_cache` disappearing after Phase 4 Landing 1)
3. **Allocator symbol total share** decreasing after copy-reduction work (Phases 5–6)
4. **Execution count** staying unchanged (printed by the benchmark itself, not by `perf`)

### Checklist For Each Phase

1. Record baseline timing: `--mode dpor --participants 4 --iterations 1 --no-crash`
2. Record baseline perf trace: `benchmarks/profile_two_phase_commit_timeout_perf.sh --participants 4`
3. Implement the change
4. Run tests: `ctest --preset debug --output-on-failure`
5. Record post-change timing (same workload)
6. Record post-change perf trace
7. Compare `elapsed_ms`, `executions`, self-hotspot shapes, and allocator share
8. Update the implementation notes in this document with the measurement results

## Bottom Line

The plan is:

- conservative first
- structural second
- explicit about the worker-local versus parallel-task boundary

Phases 1–6 are complete. The next likely target is incremental derived-state work, especially `thread_trace()` and unread-send tracking, with any further revisit-local materialization work kept perf-gated.
