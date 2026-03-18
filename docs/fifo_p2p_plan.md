# FIFO p2p Plan

This document is the single source of truth for adding FIFO peer-to-peer semantics to the current DPOR prototype.

The semantic target is the Must paper in [docs/Enea et al. - 2024 - Model Checking Distributed Protocols in Must.pdf](/home/dev/project/docs/Enea%20et%20al.%20-%202024%20-%20Model%20Checking%20Distributed%20Protocols%20in%20Must.pdf), especially Definition 3.5 and the Algorithm 1 revisit/tiebreaker discussion.

## Scope

This change adds a whole-program communication-model switch with two modes:

- `Async`
- `FifoP2P`

Mixed per-event communication models are out of scope for this patch. The current codebase does not store a model tag on send labels, and this plan intentionally avoids introducing unused API surface before the semantics are implemented and tested.

## Guiding Principles

1. Preserve correctness first.
2. Match the paper's formal FIFO consistency definition, not an informal approximation.
3. Keep the first implementation conservative where the current async shortcuts do not obviously generalize.
4. Treat optimization as a follow-up only after FIFO DPOR and oracle parity are passing.

## Semantic Target

The current async semantics already cover well-formedness and causal acyclicity. FIFO p2p adds the extra ordering constraints from Must Definition 3.5.

For this patch, implementation should follow the formal structure directly:

- shared well-formedness checks
- model-specific FIFO checks for p2p
- model-aware tiebreaker and revisit logic in DPOR

The FIFO checks should be implemented from the paper's clauses, not from ad-hoc English rules:

- clause (b): `[US]; so|dst; rf ∩ mval` is empty
- clause (c): `po; rf^{-1}; (so|dst; rf ∩ mval)` is irreflexive

Here `so` is sender order for sends from the same sender thread, and `so|dst` further restricts that order to sends with the same destination.

Selective receive remains allowed under FIFO p2p: an earlier send that does not match the receive predicate must not block consumption of a later matching send.

## Design Decisions

Accepted:

- add `CommunicationModel { Async, FifoP2P }` as a whole-program setting
- keep `SendLabelT` unchanged for now
- refactor consistency into shared validation plus model-specific checks
- keep `known_acyclic_` as a cycle-only optimization
- run FIFO checks even when `known_acyclic_` is true
- use conservative model-aware checks in the DPOR tiebreaker/revisit path where needed
- keep oracle parity as the main correctness guardrail

Rejected for this patch:

- mixed per-send communication models
- adding an unused optional model field to `SendLabelT`
- expressing FIFO as an informal pair of ad-hoc rules instead of the formal Must clauses
- preserving the current async-only masked tiebreaker shortcut for FIFO before a correct model-aware variant exists

## Roadmap

### Step 1: Add a Whole-Program Communication Model

Add a communication-model enum and thread it through:

- `dpor::algo::DporConfigT`
- model-aware consistency entry points
- oracle helpers
- tests that compare DPOR with exhaustive enumeration

This is a scoped prototype decision. The code should document that mixed-model graphs from the paper are intentionally out of scope.

### Step 2: Refactor Consistency Into Shared and Model-Specific Layers

Refactor [include/dpor/model/consistency.hpp](/home/dev/project/include/dpor/model/consistency.hpp) so that it cleanly separates:

- graph validation and issue collection for malformed or incomplete `rf`
- causal-cycle handling
- model-specific consistency logic

The existing `AsyncConsistencyCheckerT` already contains most of this structure internally. The refactor should produce a model-aware checker such as `ConsistencyCheckerT<ValueT>`.

Important constraint:

- `known_acyclic_` only proves that `(po union rf)` is acyclic
- it does not prove FIFO validity
- therefore the FIFO pass must still run even if the cycle query is skipped

No separate FIFO-valid cache should be added in the first implementation.

### Step 3: Implement FIFO p2p Consistency From the Formal Definition

Implement FIFO p2p checks directly from Must Definition 3.5 using explicit graph scans over:

- sends
- receives
- `rf` consumers
- sender order for sends from the same sender to the same destination

Recommended helper structure:

- helper to compute sender-order pairs `(s1, s2)` where `s1` and `s2` are sends by the same sender, to the same destination, and `s1` precedes `s2`
- helper to map each send to its consuming receive, if any
- helper to evaluate clause (b)
- helper to evaluate clause (c)

The implementation should name the helpers after the formal clauses where practical, so the mapping back to the paper is obvious during review.

### Step 4: Make Top-Level DPOR Consistency Checks Model-Aware

Update [include/dpor/algo/dpor.hpp](/home/dev/project/include/dpor/algo/dpor.hpp) so the DPOR hot path uses the model-aware checker instead of hardcoding async semantics.

This includes:

- `visit_if_consistent_impl`
- any helper that tests consistency before recursing
- test and oracle validation hooks that currently assume async semantics

This step is mechanically straightforward compared to the tiebreaker/revisit changes below.

### Step 5: Adapt the Tiebreaker and Revisit Path Conservatively

The hardest part of the FIFO change is the `G|Previous` tiebreaker path used by:

- `get_cons_tiebreaker`
- `get_cons_tiebreaker_masked`
- `revisit_condition`

For async, the current implementation relies on reachability shortcuts and a masked PORF view. For FIFO p2p, "consistent candidate" means:

- no causal cycle
- no FIFO violation under Definition 3.5

The first implementation should choose correctness over preserving the current shortcut:

- materialize `G|Previous` with `restrict_masked`
- remap the receive id in the restricted graph
- test each candidate send with the model-aware checker
- when evaluating revisits on restricted prefixes, tolerate only `MissingReadsFromForReceive` on unrelated receives

This keeps the FIFO path simple and reviewable even if it is more expensive than the current async-only masked shortcut.

Future optimization target:

- replace FIFO's conservative `restrict_masked` plus full-check approach with a masked model-aware tiebreaker check that avoids materializing `G|Previous`
- if that is done, it should come with targeted regression tests for `get_cons_tiebreaker_masked` and `revisit_condition`

### Step 6: Generalize RF-Rewrite Consistency Checks

The current async helper reasons only about cycle creation when rewiring a receive's `rf`.

Generalize this into a model-aware helper such as `rf_rewrite_is_consistent(model, graph, recv, send)`:

- async can keep the current reachability-based cycle shortcut
- FIFO p2p should reuse the model-aware consistency path

This helper is needed both for forward candidate checks and for revisit/tiebreaker logic.

### Step 7: Keep Forward Receive Branching Correct Before Optimizing It

In forward exploration, the receive brancher currently enumerates all unread compatible sends and relies on `VisitIfConsistent` to prune invalid children.

That remains correct under FIFO p2p, so the first implementation does not need a special forward filter.

Possible follow-up optimization:

- use the same model-aware helper to prefilter receive branches that are guaranteed FIFO-inconsistent

That should be treated as a performance improvement, not as a correctness requirement.

### Step 8: Update the Oracle to Use the Same Model

Update [tests/support/oracle_core.hpp](/home/dev/project/tests/support/oracle_core.hpp) and [tests/support/oracle.hpp](/home/dev/project/tests/support/oracle.hpp) so exhaustive enumeration runs under the configured communication model.

The oracle should remain the primary semantic cross-check for FIFO DPOR.

As with forward DPOR branching, oracle transition prefiltering is optional in the first implementation. The correctness-critical change is making the oracle checker model-aware.

### Step 9: Add Focused Regression Coverage

Add tests in [tests/consistency_test.cpp](/home/dev/project/tests/consistency_test.cpp) and [tests/dpor_test.cpp](/home/dev/project/tests/dpor_test.cpp) covering:

- FIFO clause (b) violations
- FIFO clause (c) violations
- selective receive skipping earlier non-matching sends
- non-blocking receive reading bottom under FIFO p2p
- self-send under FIFO p2p
- tiebreaker cases where `FifoP2P` and `Async` choose different candidates
- revisit-condition regressions under FIFO
- DPOR vs oracle parity under FIFO
- optionally a small paper-derived execution-count regression if it can be encoded clearly

## Main Risks

1. Implementing an informal FIFO approximation instead of the paper's formal clauses would risk unsoundness.
2. Allowing the `known_acyclic_` fast path to bypass FIFO checks would incorrectly accept FIFO-invalid graphs.
3. Reusing the async masked tiebreaker shortcut for FIFO without a formal argument would make revisit behavior hard to trust.
4. Adding speculative API surface for future mixed-model support would widen the patch without improving current semantics.

## First-Pass Success Criteria

The FIFO p2p patch is complete when all of the following are true:

- model-aware consistency checking exists for `Async` and `FifoP2P`
- FIFO checks implement the formal Must clauses
- DPOR revisit/tiebreaker logic respects FIFO semantics
- oracle parity passes under FIFO
- focused FIFO regressions pass

At that point, any masked-prefix or forward-filter optimization can be treated as follow-up work rather than part of the semantic landing.
