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
checking. It exposes an event-driven interface (`start()` + `receive()`) and
uses a minimal abstract `Environment`. The same `Coordinator` and
`Participant` classes work with both `UdpEnvironment` (real sockets) and
`SimEnvironment` (DPOR exploration).

## Protocol interface

```cpp
class Coordinator {
 public:
  bool start(Environment& env);
  bool receive(Environment& env, const Message& msg);
};

class Participant {
 public:
  bool start(Environment& env);
  bool receive(Environment& env, const Message& msg);
};
```

The environment drives protocol progress:

1. Call `start()` once.
2. While it returns/requests more input, feed messages through `receive()`.
3. Stop when `receive()` returns `false`.

## The Environment interface

```cpp
class Environment {
 public:
  virtual void send(ParticipantId destination, const Message& msg) = 0;
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

The participant calls `env.get_vote()` after processing `Prepare`. In
the simulation, `SimEnvironment` intercepts this call and produces a
`NondeterministicChoiceLabel` with choices `{"YES", "NO"}`. The DPOR engine
explores both branches. In the real UDP implementation, `UdpEnvironment`
returns a fixed vote when provided at construction time, otherwise it chooses a
random vote.

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
2. `run_and_capture()` drives the protocol directly via `start()` /
   `receive()`.
3. `SimEnvironment` fast-forwards through past I/O operations (replaying
   receive values and nondeterministic choices from the trace), then captures
   the current I/O operation as a DPOR `EventLabel`.

The cost is O(step) per call, which is negligible for small protocols.

## UDP runtime behavior

- `UdpEnvironment::run(protocol)` is single-use per environment instance.
- A background receiver thread reads datagrams and enqueues decoded messages.
- Malformed UDP datagrams are dropped (they do not crash the process).

## Verified properties

The DPOR tests check the following invariants across all explored executions:

- **Agreement**: all participants that receive a decision receive the same one.
- **Validity**: if the decision is Commit, then every participant voted Yes.
- **Crash safety**: when the coordinator crashes between phases, no participant
  receives a decision.

The UDP tests additionally check runtime transport behavior:

- message serialization/deserialization round-trips
- localhost send/receive works end to end
- full UDP protocol runs commit or abort as expected
- repeated random-vote UDP runs preserve agreement
- `UdpEnvironment::run()` is single-use
- malformed UDP datagrams are dropped without crashing the protocol

## Error handling

If the protocol implementation throws an unexpected exception during DPOR
exploration, the model checker **stops immediately**. The exception propagates
through `run_and_capture` → the `ThreadFunction` → the DPOR engine's `verify()`
call. This is a deliberate fail-fast design: a
protocol bug should not be silently absorbed and produce a false "all executions
safe" result.

The known/expected exceptions (`CrashInjected`, `StepBoundaryReached`) are
caught and handled normally. Only truly unexpected exceptions (logic errors,
assertion failures, etc.) trigger the fail-fast path.

The `Coordinator` constructor accepts a `bug_on_p1_no` flag that injects a
deliberate bug (throwing when participant 1 votes No). This exists solely to
test that the fail-fast mechanism works -- it would not be part of a real 2PC
implementation.

## Running

```bash
cmake --preset debug
cmake --build --preset debug

# Full test suite for this example file:
./build/debug/examples/two_phase_commit/dpor_two_phase_commit_test "[two_phase_commit]"

# DPOR exploration/invariants only:
./build/debug/examples/two_phase_commit/dpor_two_phase_commit_test "[two_phase_commit]~[udp]"

# UDP transport tests:
./build/debug/examples/two_phase_commit/dpor_two_phase_commit_test "[udp]"

# Scoped CTest invocation for this example directory only:
ctest --test-dir build/debug/examples/two_phase_commit
```

All tests in this file carry the `[two_phase_commit]` tag, so
`"[two_phase_commit]"` is the full example suite, not the DPOR-only subset.
The top-level form `ctest --test-dir build/debug -R "2PC|UDP"` is too broad
now that multiple example directories register tests with the same names.
The UDP-tagged tests require a runtime that permits opening localhost UDP
sockets.

## Debugging with gdb

To debug an unexpected protocol exception, catch C++ throws:

```bash
gdb ./build/debug/examples/two_phase_commit/dpor_two_phase_commit_test
(gdb) catch throw
(gdb) run "[two_phase_commit]"
(gdb) bt
```
