# Two-Phase Commit + Timers Example

This example applies the DPOR model checker to a real Two-Phase Commit (2PC)
implementation. The protocol code is runnable over real UDP. The model-checking
adapter accommodates the protocol, not the other way around.

## File overview

| File | Description |
|---|---|
| `protocol.hpp` | 2PC protocol: `Coordinator`, `Participant`, `Environment` interface |
| `udp_network.hpp` | Real UDP `Environment` implementation |
| `sim/dpor_types.hpp` | DPOR-visible value type and graph/program aliases |
| `sim/bridge.hpp` | Conversion between protocol messages/choices and DPOR values |
| `sim/crash_before_decision.hpp` | Self-contained simulation module: replay helper, environment, thread builders, `make_program()` with optional crash injection |
| `two_phase_commit_test.cpp` | Catch2 tests: DPOR invariants, protocol/timer checks, simulation checks, UDP checks |

## Design intent

The goal is to demonstrate that the DPOR engine can verify properties of
existing, production-style code. The protocol has no awareness of model
checking. It exposes an event-driven interface (`start()` + `receive()`) and
uses a minimal abstract `Environment`.

The simulation code is intentionally simple:

- `sim/bridge.hpp` and `sim/dpor_types.hpp` are shared because they define the
  DPOR encoding.
- `sim/crash_before_decision.hpp` is the only scenario header.
- `make_program()` takes an `inject_crash` option, so the same module can model
  both the no-crash executions and the crash-before-decision executions.

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
2. While it returns or requests more input, feed messages through `receive()`.
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
networking. It is a query to the external environment for a participant's vote.
Timer callbacks return the same continuation flag as `receive()`: `true` means
the protocol should keep waiting for input, `false` means it is done.

## Simulation module

The simulation header exposes one namespace and one configurable builder:

```cpp
#include "sim/crash_before_decision.hpp"

auto no_crash_program = tpc_sim::crash_before_decision::make_program({
    .num_participants = 3,
    .inject_crash = false,
});

auto crash_program = tpc_sim::crash_before_decision::make_program({
    .num_participants = 3,
    .inject_crash = true,
});
```

The module contains:

- an `Options` struct
- an `Environment`
- a `run_and_capture()`
- coordinator and participant thread builders
- a `make_program()` factory

### Votes

The participant calls `env.get_vote()` after processing `Prepare`. In the
simulation, `crash_before_decision::Environment` intercepts this call and
produces a `NondeterministicChoiceLabel` with choices `{"YES", "NO"}`. DPOR
then explores both branches.

### Crashes

The coordinator knows nothing about crashes.
`crash_before_decision::Environment` inspects `send()` calls and, when it sees
the first `DecisionMsg`, injects a `NondeterministicChoiceLabel` with choices
`{"no_crash", "crash"}` before allowing the send to proceed. If the choice
resolves to `"crash"`, the coordinator thread terminates and phase 2 never
happens. This behavior is controlled by `Options::inject_crash`, so the same
module can also run without the crash choice.

## Replay-from-scratch

The DPOR engine may call each `ThreadFunction` non-linearly across exploration
branches. The simulation module handles this with a replay-from-scratch
strategy:

1. Each `ThreadFunction` call creates a fresh protocol object and fresh
   environment.
2. `run_and_capture()` drives the protocol via `start()` and
   `receive()`.
3. The replay helper fast-forwards through past I/O operations using the trace,
   then captures the current I/O operation as a DPOR `EventLabel`.

The cost is O(step) per call, which is acceptable for this example.

## Timers in DPOR and UDP

This timeout variant extends the environment with `set_timer()` and
`cancel_timer()` and exercises timer behavior in the UDP runtime.

- The coordinator sets a vote-collection timer in `start()`. If it fires
  before all unique votes arrive, it broadcasts `DECISION ABORT` and moves to
  ack collection.
- Each participant sets a decision-wait timer after sending its vote. If it
  fires before a decision arrives, the participant locally aborts and exits.
- The coordinator sets an ack-collection timer when it enters the ack phase.
  If it fires before all unique acks arrive, the coordinator completes anyway.
- `UdpEnvironment` implements timers and dispatches callbacks inside `run()`.
- The simulation environments track active timers during replay. A wait with no
  active timer becomes a blocking receive. A wait with an active timer becomes
  a non-blocking receive, and bottom means that timer fired.
- Timer callbacks are replayed directly; the adapter does not fabricate timeout
  messages.
- The simulation assumes at most one active timer per thread at a receive
  point. The simulation environment enforces that assumption because plain
  bottom does not encode timer identity.

## UDP runtime behavior

- `UdpEnvironment::run(protocol)` is single-use per environment instance.
- Socket I/O and timer dispatch are driven on the same thread via `poll()`.
- Malformed UDP datagrams are dropped.

## Verified properties

The DPOR tests check:

- Agreement: all participants that receive a decision receive the same one.
- Validity: if the decision is Commit, then every participant voted Yes.
- Crash safety: when the coordinator crashes between phases, no participant
  receives a decision.

The protocol-state-machine tests also check:

- invalid and duplicate votes are ignored while collecting votes
- out-of-phase and duplicate acks are ignored while collecting acks
- vote-timeout expiry forces `DECISION ABORT`
- collecting all votes cancels the vote timer
- ack-timeout expiry completes the coordinator when a participant never acks
- collecting all acks cancels the ack timer
- participants arm a decision-wait timer after voting
- participants cancel that timer when a decision arrives
- participant timeout triggers a local abort

The simulation-adapter tests also check:

- waits without active timers produce blocking receives
- waits with an active timer produce non-blocking receives
- canceling the active timer returns the wait to blocking mode
- bottom replay fires the active timer callback

The UDP tests also check:

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

If protocol code throws an unexpected exception during DPOR exploration, the
scenario adapter converts that uncaught protocol exception into a terminal
`ErrorLabel`. DPOR then reports `VerifyResultKind::ErrorFound` instead of
treating the problem as infrastructure failure.

The known replay control-flow exceptions are handled internally by the
simulation module. Simulation failures still propagate as ordinary exceptions.

The `Coordinator` constructor accepts a `bug_on_p1_no` flag that injects a
deliberate bug when participant 1 votes No. This exists only to test the
verification-failure path.

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

# UDP transport tests:
./build/debug/examples/two_phase_commit_timeout/dpor_two_phase_commit_timeout_test "[udp]"

# Timer-focused tests across protocol, simulation, and UDP layers:
./build/debug/examples/two_phase_commit_timeout/dpor_two_phase_commit_timeout_test "[timer]"
```

All tests in this file carry the `[two_phase_commit]` tag, so
`"[two_phase_commit]"` is the full example suite, not the DPOR-only subset.
The UDP-tagged tests require a runtime that permits opening localhost UDP
sockets.

If you prefer `ctest`, remember that `ctest -R ...` matches CTest test names,
not Catch tags. For this executable, use
`ctest --preset debug -R dpor_two_phase_commit_timeout_test` to run the whole
discovered set, and run the test binary directly when you want Catch tag
filters such as `[udp]` or `[timer]`.

## Debugging with gdb

```bash
gdb ./build/debug/examples/two_phase_commit_timeout/dpor_two_phase_commit_timeout_test
(gdb) catch throw
(gdb) run "[two_phase_commit]"
(gdb) bt
```
