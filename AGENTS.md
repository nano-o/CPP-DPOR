# AGENTS.md

## Project Direction

This project implements a DPOR model-checker inspired by:

- `docs/Enea et al. - 2024 - Model Checking Distributed Protocols in Must.pdf`

The Must paper is our semantic north star for:

- execution graphs (`E`, `po`, `rf`)
- well-formedness and consistency concepts
- async communication semantics and later DPOR exploration strategy

## Important Constraint: Existing Codebases

Unlike Must, we are not designing a new language/API from scratch.
Our target is applying DPOR to existing production codebases (in particular, `stellar-core`), where:

- events are often less structured
- message schemas and handlers already exist
- receive compatibility may only be known by running existing processing logic

## Practical Modeling Rules

To support the above:

- prefer adapter/extraction layers that map runtime behavior into execution-graph events
- avoid hard-coding assumptions that all receives are declaratively enumerable
- support receive matching via predicates/callables, not only finite value sets
- allow matching to defer to system logic (e.g., reject values that produce `INVALID`)

## Determinism Requirements

Any callback/predicate used during exploration (especially receive matching) must be:

- deterministic for the same exploration state
- side-effect free, or run against isolated snapshots

This is required to preserve DPOR soundness/completeness assumptions.

## Current Scope

- prioritize correctness and clarity over optimization
- focus first on async communication model support
- add genericity where it improves integration with real systems (not genericity for its own sake)

### Semantics In Scope (Now)

- communication model: async message passing only
- receive semantics: blocking receives only
- event kinds: send, receive, nondeterministic choice, block, error
- receive compatibility: predicate/callable-based matching
- exploration strategy: insertion-order execution graphs + backward revisiting

### Semantics Out of Scope (Now)

- non-blocking receives / bottom (`⊥`) receive results
- multiple communication models (`p2p`, `cd`, `mbox`) and their model-specific consistency rules
- monitor-specific semantics for temporal properties

## Consistency Invariants Policy

- Every graph that is recursively explored by DPOR (`visit`) must satisfy full consistency (`consistent(G)`), with no issue-code exemptions.
- Intermediate helper constructions (e.g., transient graphs during restrict/remap/revisit computation) may be partial only as internal artifacts.
- No helper/partial graph may be emitted as a complete execution, or used as an exploration state, unless it passes full consistency.

## Prototype Policy

This codebase is currently a prototype.

- there is no backward-compatibility commitment
- prioritize clean iteration over preserving old APIs
- when better structure is identified, prefer replacing APIs instead of layering compatibility shims
