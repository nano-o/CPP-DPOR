# Two-Phase Commit + Timers Example

This example applies the DPOR model checker to a real Two-Phase Commit (2PC)
implementation. The protocol code is runnable over real UDP -- the
model-checking adapter accommodates the protocol, not the other way around.

## File overview

| File                          | Description                                                                        |
|-------------------------------|------------------------------------------------------------------------------------|
| `protocol.hpp`                | 2PC protocol: `Coordinator`, `Participant`, `Environment` interface                |
| `udp_network.hpp`             | Real UDP `Environment` implementation                                              |
| `simulation.hpp`              | Umbrella header for the DPOR-facing simulation modules                             |
| `sim/dpor_types.hpp`          | DPOR-visible value and graph/program aliases                                       |
| `sim/bridge.hpp`              | Conversion between protocol messages/choices and DPOR values                       |
| `sim/core.hpp`                | Shared deterministic replay/capture helper                                         |
| `sim/nominal_environment.hpp` | Explicit nondeterministic-vote scenario environment                                |
| `sim/crash_environment.hpp`   | Explicit crash-before-decision scenario environment                                |
| `sim/programs.hpp`            | Scenario-specific thread-function and program builders                             |
| `two_phase_commit_test.cpp`   | Catch2 tests: DPOR invariant checks + UDP integration tests + timer behavior tests |

## Design intent

The goal is to demonstrate that the DPOR engine can verify properties of
**existing, production-style code**. The protocol has no awareness of model
checking. It exposes an event-driven interface (`start()` + `receive()`) and
uses a minimal abstract `Environment`. The same `Coordinator` and
`Participant` classes work with both `UdpEnvironment` (real sockets) and the
explicit simulation environments used for DPOR exploration.

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
class Environment;

using TimerId = std::size_t;
using TimerCallback = std::function<bool(Environment&)>;

class Environment {
 public:
  virtual void send(ParticipantId destination, const Message& msg) = 0;
  virtual Vote get_vote() = 0;
  virtual void set_timer(TimerId id, std::size_t timeout_ms,
                         TimerCallback callback) = 0;
  virtual void cancel_timer(TimerId id) = 0;
};
```

This is called `Environment` rather than `Network` because `get_vote()` is not
a networking operation -- it's a query to the external environment for a
participant's vote decision. Timer operations are also part of the environment
contract. Timer callbacks return the same continuation flag as `receive()`:
`true` means the protocol should keep waiting for input, `false` means it is
done.

## Nondeterminism

There are two sources of nondeterminism in 2PC: participant votes and
coordinator crashes. Both are modeled as DPOR events that occur at the correct
causal point in the protocol's execution, rather than being pre-determined at
construction time.

### Votes

The participant calls `env.get_vote()` after processing `Prepare`. In
the simulation, `NondeterministicVoteEnvironment` intercepts this call and
produces a `NondeterministicChoiceLabel` with choices `{"YES", "NO"}`. The
DPOR engine explores both branches. In the real UDP implementation,
`UdpEnvironment` returns a fixed vote when provided at construction time,
otherwise it chooses a random vote.

### Crashes

The coordinator knows nothing about crashes.
`CrashBeforeDecisionEnvironment` inspects every `send()` call and, when it
sees the first `DecisionMsg` (the boundary between phase 1 and phase 2),
injects a `NondeterministicChoiceLabel` with choices `{"no_crash", "crash"}`
before allowing the send to proceed. If the choice resolves to `"crash"`, the
coordinator's thread is terminated and phase 2 never happens. This models a
crash between the two phases without any protocol-level awareness.

## Simulation module split

The simulation code is intentionally split into three concerns:

- `sim/dpor_types.hpp`: the compact value type and aliases exposed to DPOR
- `sim/bridge.hpp`: the bridge between protocol messages and DPOR values
- `sim/core.hpp`: deterministic replay, target-step capture, timer replay, and
  protocol-exception handling

The scenario files stay small and explicit. They delegate the tricky replay
mechanics to `ReplayCore`, but each scenario still spells out its own policy
for `send()` and `get_vote()`.

## The replay-from-scratch approach

The DPOR engine may call each `ThreadFunction` non-linearly across exploration
branches. The simulation handles this with a simple replay strategy:

1. Each `ThreadFunction` call creates a fresh protocol object and
   scenario environment.
2. `run_and_capture()` drives the protocol directly via `start()` /
   `receive()`.
3. `ReplayCore` fast-forwards through past I/O operations (replaying receive
   values and nondeterministic choices from the trace), then captures the
   current I/O operation as a DPOR `EventLabel`.

The cost is O(step) per call, which is negligible for small protocols.

## Timers in DPOR and UDP

This timeout variant extends the environment with `set_timer()` /
`cancel_timer()` and exercises timer behavior in the UDP runtime.

- The coordinator sets a vote-collection timer in `start()`. If the timer
  fires before all unique participant votes arrive, it broadcasts
  `DECISION ABORT` and transitions to ack collection.
- Each participant sets a decision-wait timer immediately after sending its
  vote. If that timer fires before any decision arrives, the participant
  locally aborts and exits. This is intentionally unsafe and exists only to
  exercise timeout behavior in the example.
- The coordinator sets an ack-collection timer when it transitions to the ack
  phase. If the timer fires before all unique acks arrive, the coordinator
  completes without waiting further. This prevents the coordinator from hanging
  when a participant times out and never sends its ack.
- `UdpEnvironment` implements timers and dispatches callbacks inside `run()`.
- `ReplayCore` tracks active timers during replay. When a thread waits for
  input with no active timer, the adapter emits a blocking receive. When a
  timer is active, the adapter emits a non-blocking receive, and a bottom
  observation means that active timer fired.
- Timer callbacks are replayed directly; the adapter does not fabricate special
  timeout messages to wake the protocol.
- This example assumes at most one active timer per thread at a receive point.
  The simulation enforces that assumption and throws if the protocol violates
  it, because plain bottom does not encode timer identity.
- DPOR exploration in this example therefore models message, vote, crash, and
  timer races through the existing receive/bottom semantics.

## UDP runtime behavior

- `UdpEnvironment::run(protocol)` is single-use per environment instance.
- Socket I/O and timer dispatch are driven on the same thread via `poll()`.
- Malformed UDP datagrams are dropped (they do not crash the process).

## Verified properties

The DPOR tests check the following invariants across all explored executions:

- **Agreement**: all participants that receive a decision receive the same one.
- **Validity**: if the decision is Commit, then every participant voted Yes.
- **Crash safety**: when the coordinator crashes between phases, no participant
  receives a decision.

The protocol-state-machine tests additionally check:

- invalid and duplicate votes are ignored while collecting votes
- out-of-phase and duplicate acks are ignored while collecting acks
- vote-timeout expiry forces `DECISION ABORT`
- collecting all votes cancels the vote timer
- ack-timeout expiry completes the coordinator when a participant never acks
- collecting all acks cancels the ack timer
- participants arm a decision-wait timer after voting
- participants cancel that timer when a decision arrives
- participant timeout triggers a local abort

The simulation-adapter tests additionally check:

- waits without active timers produce blocking receives
- waits with an active timer produce non-blocking receives
- canceling the active timer returns the wait to blocking mode
- bottom replay fires the active timer callback

The UDP tests additionally check runtime transport and timer behavior:

- message serialization/deserialization round-trips
- localhost send/receive works end to end
- full UDP protocol runs commit or abort as expected
- repeated random-vote UDP runs preserve agreement
- a participant can locally abort if no decision arrives before its timer fires
- timer callback fires without incoming UDP traffic
- canceled timer does not fire
- replacing a timer id keeps only the newest callback
- shutdown is clean even with pending timers

## Error handling

If the protocol implementation throws an unexpected exception during DPOR
exploration, the simulation adapter converts that uncaught protocol exception
into a terminal `ErrorLabel`. DPOR then reports
`VerifyResultKind::ErrorFound`, treating the bug as a verification failure in
the explored execution rather than as an infrastructure crash.

The known/expected replay exceptions (`ScenarioThreadTerminated`,
`StepBoundaryReached`) are caught and handled normally. Simulator/adaptor
failures still propagate as ordinary exceptions; only unexpected protocol
exceptions (logic errors, assertion failures, etc.) are reclassified as
verification failures.

The `Coordinator` constructor accepts a `bug_on_p1_no` flag that injects a
deliberate bug (throwing when participant 1 votes No). This exists solely to
test that the verification-failure path works -- it would not be part of a real
2PC implementation.

## Running

```bash
cmake --preset debug
cmake --build --preset debug

# Full test suite for this example file:
./build/debug/examples/two_phase_commit_timeout/dpor_two_phase_commit_timeout_test "[two_phase_commit]"

# DPOR exploration/invariants only:
./build/debug/examples/two_phase_commit_timeout/dpor_two_phase_commit_timeout_test "[two_phase_commit]~[protocol]~[simulation]~[udp]"

# Protocol-state-machine checks, including coordinator and participant timeouts:
./build/debug/examples/two_phase_commit_timeout/dpor_two_phase_commit_timeout_test "[protocol]"

# Simulation-adapter checks for timer/replay behavior:
./build/debug/examples/two_phase_commit_timeout/dpor_two_phase_commit_timeout_test "[simulation]"

# UDP transport tests (this also includes the UDP timer tests):
./build/debug/examples/two_phase_commit_timeout/dpor_two_phase_commit_timeout_test "[udp]"

# Timer-focused tests across protocol, simulation, and UDP layers:
./build/debug/examples/two_phase_commit_timeout/dpor_two_phase_commit_timeout_test "[timer]"
```

All tests in this file carry the `[two_phase_commit]` tag, so
`"[two_phase_commit]"` is the full example suite, not the DPOR-only subset.
The UDP-tagged tests require a runtime that permits opening localhost UDP
sockets.

## Debugging with gdb

To debug an unexpected protocol exception, catch C++ throws:

```bash
gdb ./build/debug/examples/two_phase_commit_timeout/dpor_two_phase_commit_timeout_test
(gdb) catch throw
(gdb) run "[two_phase_commit]"
(gdb) bt
```
