# Two-Phase Commit Example

This example applies the DPOR model checker to a real Two-Phase Commit (2PC)
implementation. The protocol code is runnable over real UDP -- the
model-checking adapter accommodates the protocol, not the other way around.

## File overview

| File | Description |
|------|-------------|
| `protocol.hpp` | 2PC protocol: `Coordinator`, `Participant`, `Environment` interface |
| `udp_network.hpp` | Real UDP `Environment` implementation |
| `simulation.hpp` | DPOR adapter: `SimEnvironment` + `ThreadFunction` factories |
| `two_phase_commit_test.cpp` | Catch2 tests: DPOR invariant checks + UDP integration tests |

## Design intent

The goal is to demonstrate that the DPOR engine can verify properties of
**existing, production-style code**. The protocol has no awareness of model
checking -- it calls `env.send()`, `env.receive()`, and `env.get_vote()` on an
abstract `Environment` interface. The same `Coordinator` and `Participant`
classes work with both `UdpEnvironment` (real sockets) and `SimEnvironment`
(DPOR exploration).

## The Environment interface

```cpp
class Environment {
 public:
  virtual void send(ParticipantId destination, const Message& msg) = 0;
  virtual Message receive() = 0;
  virtual Vote get_vote() = 0;
};
```

This is called `Environment` rather than `Network` because `get_vote()` is not
a networking operation -- it's a query to the external environment for a
participant's vote decision.

## Nondeterminism

There are two sources of nondeterminism in 2PC: participant votes and
coordinator crashes. Both are modeled as DPOR events that occur at the correct
causal point in the protocol's execution, rather than being pre-determined at
construction time.

### Votes

The participant calls `env.get_vote()` after receiving a Prepare message. In
the simulation, `SimEnvironment` intercepts this call and produces a
`NondeterministicChoiceLabel` with choices `{"YES", "NO"}`. The DPOR engine
explores both branches. In the real UDP implementation, `UdpEnvironment` simply
returns a fixed vote passed at construction time.

### Crashes

The coordinator knows nothing about crashes. `SimEnvironment` inspects every
`send()` call and, when it sees the first `DecisionMsg` (the boundary between
phase 1 and phase 2), it injects a `NondeterministicChoiceLabel` with choices
`{"no_crash", "crash"}` before allowing the send to proceed. If the choice
resolves to "crash", the coordinator's thread is terminated and phase 2 never
happens. This models a crash between the two phases without any protocol-level
awareness.

## The replay-from-scratch approach

The DPOR engine is single-threaded and calls each `ThreadFunction`
non-linearly across exploration branches. The simulation handles this with a
simple replay strategy:

1. Each `ThreadFunction` call creates a fresh protocol object and
   `SimEnvironment`.
2. `run()` is launched in a real OS thread.
3. `SimEnvironment` fast-forwards through past I/O operations (replaying
   receive values and nondeterministic choices from the trace), then captures
   the current I/O operation as a DPOR `EventLabel`.
4. The DPOR thread collects the label and tears down the OS thread.

The cost is O(step) per call, which is negligible for small protocols.

## Verified properties

The tests check the following invariants across all DPOR-explored executions:

- **Agreement**: all participants that receive a decision receive the same one.
- **Validity**: if the decision is Commit, then every participant voted Yes.
- **Crash safety**: when the coordinator crashes between phases, no participant
  receives a decision.

## Running

```bash
cmake --preset debug
cmake --build --preset debug
ctest --test-dir build/debug -R "2PC|UDP"
```
