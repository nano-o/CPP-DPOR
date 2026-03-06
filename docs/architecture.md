# Architecture

This document describes the high-level architecture of the `dpor` library, a Dynamic Partial Order Reduction (DPOR) checker for distributed protocols.

The system is organized into three primary layers: **Model**, **Algorithm**, and **API**.

## 1. Model Layer (`dpor::model`)

The model layer defines the formal representation of concurrent executions and their properties.

### Events and Traces
- **`EventT`**: The fundamental unit of execution. Events include `Send`, `Receive`, `ND` (non-deterministic choice), `Block` (an internal receive-wait marker inserted by DPOR), and `Error` (assertion or invariant violation). Receive labels can be blocking or non-blocking. Each event is associated with a thread and a unique identifier.
- **`ExecutionGraphT`**: Represents a single execution of the system. It stores a set of events and the **Reads-From (RF)** relation, which maps receive events either to their corresponding send event or to bottom (`⊥`) for a non-blocking receive that did not consume a send.

### Relations and Reachability
- **`Relation`**: Defines the **Program Order (PO)** and **Happens-Before (HB)** relations.
- **`ExplorationGraphT`**: A specialized version of the execution graph used during DPOR exploration. It includes a **`PorfCache`** (based on vector clocks) that provides $O(1)$ reachability queries for the partial order.
- **`Consistency`**: The `AsyncConsistencyCheckerT` validates that an execution graph is "consistent" (e.g., no causal cycles, matching send/receive values, no multiple consumes of a single send, and no blocking receive reading bottom).

## 2. Algorithm Layer (`dpor::algo`)

The algorithm layer implements the core DPOR engine and the system-under-test (SUT) model.

### Program Representation
- **`ProgramT`**: Represents the system being checked. It consists of a fixed set of **Thread Functions**, where each thread is a deterministic function of its observed trace. The thread set is defined upfront and cannot change during exploration — there is no dynamic thread creation. To model systems where threads are spawned at runtime, pre-declare all potential threads and use control flow (e.g. a nondeterministic choice or an initial receive) to keep them idle until "spawned".
- **`ThreadFunctionT`**: A function that takes a thread's local history and returns the next event it will perform. The history is a sequence of `ObservedValueT<ValueT>` entries: receive outcomes (payload or bottom for non-blocking receives) and nondeterministic choices. It does not include send/block/error events. The separate `step` argument is therefore required to represent local control-flow progress. Thread functions should not emit `Block`; DPOR inserts `Block` internally when a blocking receive has no compatible unread send.

### DPOR Engine
- **`dpor.hpp`**: Implements the core DPOR algorithm based on the "revisiting" approach (Enea et al., 2024).
- **`verify()` result**: DPOR reports `AllExecutionsExplored`, `ErrorFound`, or `DepthLimitReached`.
- **Exploration**: The engine recursively explores the space of consistent executions. It uses **backward revisiting** to identify alternative interleavings or message matches that could lead to new behaviors, performs Must-style receive rescheduling before terminating an execution, and explores non-blocking receives both through compatible sends and the bottom branch.

## 3. API Layer (`dpor::api`)

The API layer provides the stable integration surface for users of the library.

- **`Session`**: The primary entry point. A session configures the exploration (e.g., max depth, name) and manages the execution of the DPOR algorithm.
- **`SessionConfig`**: Allows users to tune parameters like resource limits and exploration strategies.

## 4. Demonstration and Examples (`examples/`)

The library includes examples that demonstrate how to model distributed protocols:
- **`minimal/`**: A basic example of setting up a `ProgramT` and running a DPOR session.
- **`two_phase_commit/`**: A more complex case study modeling the Two-Phase Commit (2PC) protocol, including UDP network modeling and simulation logic.
- **`two_phase_commit_timeout/`**: A 2PC variant that adds timers in the UDP runtime while keeping current DPOR exploration focused on async message interleavings.

---

## Design Principles

- **Immutability**: Exploration graphs are treated as mostly immutable or copy-on-write to simplify the backtracking logic in the DPOR algorithm.
- **Determinism**: Given a fixed seed and program, the exploration is fully deterministic.
- **Separation of Concerns**: The consistency rules (what makes an execution valid) are decoupled from the DPOR algorithm (how to find all valid executions).
- **Zero Global State**: All state is encapsulated within `Session` or passed explicitly through the exploration recursion.
