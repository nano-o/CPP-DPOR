# Merged Performance Optimization Plan

This document merges:

- `docs/performance_optimization_plan.md`
- `docs/exploration_graph_incremental_branching_plan.md`

The merged plan keeps the low-risk wins from the former, keeps the larger structural direction from the latter, and treats future parallel search as a hard constraint.

## Constraint

Different parallel `visit()` calls must not share mutable exploration state.

That implies two distinct execution contexts:

- worker-local recursion, where temporary mutation plus rollback is allowed
- task-spawn boundaries, where a worker must hand off an isolated snapshot

Today, the by-value `visit()` / `visit_if_consistent()` style is already the natural task-spawn boundary interface: a caller can hand an owned graph to a worker without sharing mutable state. If local rollback is introduced later, that should remain an internal optimization inside one worker rather than a change to the spawn-boundary ownership model.

## Agreed Diagnosis

Both plans agree on the main performance story:

1. graph copying and allocator churn are the largest costs
2. consistency checking and cycle detection rebuild too much state
3. backward revisit amplifies graph-materialization cost
4. the timeout adapter's string parsing is real, but secondary to DPOR-core costs

## Guiding Principles

1. Preserve correctness first.
2. Preserve parallel-task isolation at all times.
3. Land low-risk wins before invasive refactors.
4. Do not optimize by assuming stronger invariants than the current API actually guarantees.

## Changes To Accept

The merged plan accepts these ideas from `performance_optimization_plan.md`:

- use a structured integer or enum `ValueT` in the 2PC timeout benchmark
- replace clearly dense event-ID-indexed maps with flat vectors where semantics allow it
- remove duplicate cycle work carefully
- reserve obvious hot vectors
- clean up move/copy usage in `visit()` where ownership already allows it

The merged plan accepts these ideas from `exploration_graph_incremental_branching_plan.md`:

- distinguish worker-local branching from parallel task boundaries
- add explicit task-snapshot semantics if local mutation is introduced
- treat `restrict()` and `with_rf()` as special revisit-heavy cases
- stage the refactor so rollback machinery is validated before recursion changes

## Changes To Reject

The merged plan rejects these simplifications:

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

Land the least risky benchmark-specific changes first:

1. Replace the timeout example's `std::string` message `ValueT` with a structured enum or integer encoding.
2. Remove text deserialization from the simulation path.
3. Keep the public library API unchanged; limit this phase to the example and benchmark wiring.

Expected result:

- less string allocation and copying
- lower replay overhead in `run_and_capture()` and `sim_receive()`

### Phase 2: Safe Representation Cleanups In Core Types

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

(see Bottleneck #3 in `./performance_optimization_plan.md`)

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
   - `restrict()`, `with_rf()`, `with_rf_source()`, and `with_bottom_rf()` should all produce graphs with unknown acyclicity

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
- `restrict()` and `with_rf()` clear known acyclicity
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
- Conclusion: Landing 1 appears structurally correct but not sufficient for end-to-end speedup on this workload. Landing 2 remains the right next step, because full PORF construction / topological work still dominates the DPOR-specific cost cluster.

#### Landing 2: Warm-Cache Incremental PORF Extension

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

Measurement plan:

1. land Landing 1
2. run targeted tests, then `ctest --preset debug --output-on-failure`
3. rerun the timeout benchmark and `perf`
4. only then land Landing 2
5. rerun the same benchmark and `perf` again before considering Phase 5

### Phase 5: Eliminate Common-Case Branch Copies In One Worker

After the Phase 4 measurements, this is now the most plausible next major step. The profile is increasingly allocator/materialization-heavy, and the clone-and-extend PORF path did not buy enough runtime to justify keeping the extra machinery, let alone doing more PORF-local refinement before addressing branch-state copying more directly.

If phases 1 through 4 still leave graph-copying dominant after re-measurement, move to the larger structural change:

1. add checkpoint and rollback support to `ExplorationGraphT`
2. keep that rollback state worker-local
3. rewrite the common forward branches in `visit()` to mutate, recurse, and roll back
4. keep explicit isolated snapshot creation at any future parallel task boundary

This is the point where the plan begins to use local mutation, but only inside one worker's recursion stack.

This phase is intentionally limited to the common forward path. The send-handling path still calls `backward_revisit()`, and that code currently depends on `restrict()` plus `with_rf()` over remapped event IDs. Those revisit-specific paths should continue to create independent materialized state until Phase 6 provides a safer replacement.

If this phase is implemented, keep the by-value ownership model at the task-spawn boundary. A reasonable shape is a by-value spawn-facing wrapper that calls an internal by-reference recursive helper inside one worker.

Expected result:

- largest reduction in ordinary branch-state materialization
- preserved compatibility with future parallel search through explicit task snapshots

### Phase 6: Tackle Revisit-Specific Materialization

Only after the forward path is cheaper should revisit-heavy structure be attacked:

1. reduce temporary `with_rf()` copies
2. introduce a restricted view for revisit checks if needed
3. keep compact graph materialization only where the algorithm genuinely needs independent restricted state

This phase is intentionally later because it is more complex and easier to get wrong.

## Suggested Landing Order

1. example-local structured `ValueT`
2. dense storage for clearly dense event-ID-indexed fields
3. trivial extra-copy and missed-move fixes in `visit()`
4. unify cycle work without changing diagnostics
5. re-measure and confirm whether the dominant remaining costs are cycle/PORF, unread-send scanning, or graph materialization
6. Phase 4 Landing 1: internal known-acyclic propagation for the append-only forward path, so the checker can skip cold cycle queries there
7. re-measure, then optionally prototype Phase 4 Landing 2: warm-cache PORF clone-and-extend for the same append-only forward path, but keep it only if it clears a clear perf/complexity bar
8. worker-local rollback plus explicit task snapshots if graph-copy/materialization costs are still dominant after the completed Phase 4 measurements; this is now the recommended next step
9. revisit-specific restricted views or other deeper structural work only if revisit-heavy materialization still dominates after the forward path is cheaper
10. reserve hot vectors and other minor adjacency-building cleanups where perf justifies them

## Why This Order

This order balances:

- immediate wins
- implementation safety
- compatibility with future parallel search
- preservation of current correctness and diagnostics

It also avoids paying the complexity cost of a rollback-based redesign before the cheaper and more local wins have been measured.

After the Phase 2 results, this ordering was already more strongly justified: the event-ID cleanup removed a large chunk of graph-copy overhead, so Phases 5 and 6 needed to stay perf-gated. After the completed Phase 4 measurements, that gating now points toward Phase 5: Phase 4 fixed the forward-path cycle/PORF structure cleanly, but the remaining profile is still too allocator-heavy and materialization-heavy for more PORF-local work to be the best next bet. The reverted Landing 2 prototype reinforces that conclusion.

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

## Bottom Line

The merged plan is:

- conservative first
- structural second
- explicit about the worker-local versus parallel-task boundary

It keeps the good benchmark-specific and representation-local wins from the updated `performance_optimization_plan.md`, but it also preserves the stronger long-term path of worker-local mutation plus explicit task snapshots if graph copying remains the dominant cost after the cheap wins land.
