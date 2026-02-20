# AGENTS.md

## Project Direction

This project implements a DPOR model-checker inspired by:

- `docs/Enea et al. - 2024 - Model Checking Distributed Protocols in Must.pdf`

The Must paper is our semantic north star for:

- execution graphs (`E`, `po`, `rf`)
- well-formedness and consistency concepts
- p2p communication semantics and later DPOR exploration strategy

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
- focus first on p2p communication model support
- add genericity where it improves integration with real systems (not genericity for its own sake)
