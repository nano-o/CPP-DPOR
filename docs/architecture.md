# Architecture

This document describes the high-level architecture of the `dpor` library, a Dynamic Partial Order Reduction (DPOR) checker for distributed protocols.

The system is organized into three primary layers: **Model**, **Algorithm**, and **API**.

## 1. Model Layer (`dpor::model`)

The model layer defines the formal representation of concurrent executions and their properties.

### Events and Traces
- **`EventT`**: The fundamental unit of execution. Events include `Send`, `Receive`, `ND` (non-deterministic choice), `Block` (an internal receive-wait marker inserted by DPOR), and `Error` (assertion or invariant violation). Each event is associated with a thread and a unique identifier.
- **`ExecutionGraphT`**: Represents a single execution of the system. It stores a set of events and the **Reads-From (RF)** relation, which maps receive events to their corresponding send events.

### Relations and Reachability
- **`Relation`**: Defines the **Program Order (PO)** and **Happens-Before (HB)** relations.
- **`ExplorationGraphT`**: A specialized version of the execution graph used during DPOR exploration. It includes a **`PorfCache`** (based on vector clocks) that provides $O(1)$ reachability queries for the partial order.
- **`Consistency`**: The `AsyncConsistencyCheckerT` validates that an execution graph is "consistent" (e.g., no causal cycles, matching send/receive values, no multiple consumes of a single send).

## 2. Algorithm Layer (`dpor::algo`)

The algorithm layer implements the core DPOR engine and the system-under-test (SUT) model.

### Program Representation
- **`ProgramT`**: Represents the system being checked. It consists of a set of **Thread Functions**, where each thread is a deterministic function of its observed trace.
- **`ThreadFunctionT`**: A function that takes a thread's local history and returns the next event it will perform. Thread functions should not emit `Block`; DPOR inserts `Block` internally when a blocking receive has no compatible unread send.

### DPOR Engine
- **`dpor.hpp`**: Implements the core DPOR algorithm based on the "revisiting" approach (Enea et al., 2024).
- **Exploration**: The engine recursively explores the space of consistent executions. It uses **backward revisiting** to identify alternative interleavings or message matches that could lead to new behaviors, and performs Must-style receive rescheduling before terminating an execution.

## 3. API Layer (`dpor::api`)

The API layer provides the stable integration surface for users of the library.

- **`Session`**: The primary entry point. A session configures the exploration (e.g., max depth, name) and manages the execution of the DPOR algorithm.
- **`SessionConfig`**: Allows users to tune parameters like resource limits and exploration strategies.

## 4. Demonstration and Examples (`examples/`)

The library includes examples that demonstrate how to model distributed protocols:
- **`minimal/`**: A basic example of setting up a `ProgramT` and running a DPOR session.
- **`two_phase_commit/`**: A more complex case study modeling the Two-Phase Commit (2PC) protocol, including UDP network modeling and simulation logic.

---

## Design Principles

- **Immutability**: Exploration graphs are treated as mostly immutable or copy-on-write to simplify the backtracking logic in the DPOR algorithm.
- **Determinism**: Given a fixed seed and program, the exploration is fully deterministic.
- **Separation of Concerns**: The consistency rules (what makes an execution valid) are decoupled from the DPOR algorithm (how to find all valid executions).
- **Zero Global State**: All state is encapsulated within `Session` or passed explicitly through the exploration recursion.
