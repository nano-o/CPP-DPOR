# Benchmarks

This directory contains standalone benchmark executables for the two-phase
commit examples. They do not use Catch2 and can be built with
`DPOR_BUILD_TESTING=OFF`, so their timings reflect the DPOR/oracle work rather
than test harness overhead.

## Available benchmarks

- `dpor_two_phase_commit_timeout_benchmark`
  2PC model with timers, using
  `examples/two_phase_commit_timeout/sim/crash_before_decision.hpp` with
  `Options::inject_crash` toggled by the CLI.

The executable supports the following CLI:

```text
--mode dpor|oracle|both
--participants N
--iterations N
--no-crash
--fifo
--parallel
--max-workers N
--max-queued-tasks N
--spawn-depth-cutoff N
--min-fanout N
```

- `--mode dpor` measures only DPOR exploration.
- `--mode oracle` measures only the exhaustive oracle.
- `--mode both` runs both and checks that they agree on the full-execution count.
- `--participants` defaults to `3`.
- `--iterations` defaults to `1`.
- `--no-crash` disables the coordinator crash choice so you can benchmark the
  no-crash state space separately.
- `--fifo` switches the communication model from async to FIFO point-to-point
  (`CommunicationModel::FifoP2P`) for both DPOR and oracle runs.
- `--parallel` switches DPOR runs from `verify()` to `verify_parallel()`. If
  `--max-workers` is omitted, worker count falls back to the library's
  hardware-based default.
- `--max-workers`, `--max-queued-tasks`, `--spawn-depth-cutoff`, and
  `--min-fanout` pass through to `ParallelVerifyOptions`. Supplying any of
  these also enables `--parallel`.
- Parallel flags are ignored in `--mode oracle`.

Benchmark output reports `terminal_executions`, `full_executions`,
`error_executions`, and `depth_limit_executions` for each run. Oracle runs only
enumerate full executions, so their error and depth-limit counts remain zero.

When the oracle runs, the benchmark also prints `paths_explored`, which counts
raw oracle DFS terminal paths before deduplication. That number is typically
much larger than the final `full_executions` count.

## Configure and build

To build only the library, examples, and benchmarks in an optimized
configuration, without Catch2 or any test targets:

```bash
cmake -S . -B build/bench-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DDPOR_BUILD_TESTING=OFF \
  -DDPOR_BUILD_EXAMPLES=ON \
  -DDPOR_BUILD_BENCHMARKS=ON

cmake --build build/bench-release --target \
  dpor_two_phase_commit_timeout_benchmark
```

## Perf profiling

If you want a useful `perf` capture, use a separate profiling-oriented build so
you keep optimization but also get debug info and reliable stacks:

```bash
cmake -S . -B build/perf -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDPOR_BUILD_TESTING=ON \
  -DDPOR_BUILD_EXAMPLES=ON \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls"

cmake --build build/perf --target dpor_two_phase_commit_timeout_benchmark -j
```

Then record with a software clock event and DWARF unwinding. This avoids the
hybrid `cpu_core` / `cpu_atom` split and usually produces cleaner user-space
call stacks:

```bash
perf record -e cpu-clock --all-user -F 499 \
  --call-graph dwarf,4096 \
  -o perf-clean.data -- \
  build/perf/benchmarks/two_phase_commit_timeout/dpor_two_phase_commit_timeout_benchmark \
  --mode dpor --participants 3 --iterations 1
```

If the sample count is too low, rerun the same command with a larger workload,
for example `--iterations 3`.

Useful follow-up reports:

```bash
perf report -i perf-clean.data --stdio --children -g graph,0.5,caller > perf-clean.report.txt

perf report -i perf-clean.data --stdio --no-children --percent-limit 0.5
```

If stacks still look truncated, increase the kernel callchain depth before
recording:

```bash
sudo sysctl -w kernel.perf_event_max_stack=512
```

## Run examples

2PC, DPOR only:

```bash
build/bench-release/benchmarks/two_phase_commit_timeout/dpor_two_phase_commit_timeout_benchmark \
  --mode dpor --participants 3 --iterations 1
```

2PC, FIFO DPOR only:

```bash
build/bench-release/benchmarks/two_phase_commit_timeout/dpor_two_phase_commit_timeout_benchmark \
  --mode dpor --fifo --participants 3 --iterations 1
```

2PC, parallel DPOR with conservative split heuristics:

```bash
build/bench-release/benchmarks/two_phase_commit_timeout/dpor_two_phase_commit_timeout_benchmark \
  --mode dpor --participants 4 --iterations 1 --no-crash \
  --parallel --max-workers 8 --max-queued-tasks 8 \
  --spawn-depth-cutoff 2 --min-fanout 4
```

2PC, oracle only:

```bash
build/bench-release/benchmarks/two_phase_commit_timeout/dpor_two_phase_commit_timeout_benchmark \
  --mode oracle --participants 2 --iterations 1
```

## Output

Each benchmark prints per-run timings and a summary. Example shape:

```text
Plain 2PC benchmark participants=2 communication_model=async inject_crash=true iterations=1 progress_interval_ms=1000 optimized_build=true
DPOR
  progress run 1: elapsed_ms=1000.000 terminal_executions=12 full_executions=12 error_executions=0 depth_limit_executions=0 active_workers=1/1 queued_tasks=0/0 counts_exact=true
  run 1: terminal_executions=24 full_executions=24 error_executions=0 depth_limit_executions=0 elapsed_ms=...
  summary: min_ms=... avg_ms=... max_ms=... terminal_executions=24 full_executions=24 error_executions=0 depth_limit_executions=0
Oracle
  run 1: terminal_executions=24 full_executions=24 error_executions=0 depth_limit_executions=0 paths_explored=... elapsed_ms=...
  summary: min_ms=... avg_ms=... max_ms=... terminal_executions=24 full_executions=24 error_executions=0 depth_limit_executions=0 paths_explored=...
```

Use `--iterations N` to get repeated timings in a single process.
Parallel runs print the selected `ParallelVerifyOptions` on the header line.
Use `--progress-interval-ms N` to control DPOR progress output cadence. The
benchmark defaults to `1000`, and `0` disables benchmark progress lines.
This is a benchmark-harness convention: the raw DPOR API treats
`progress_report_interval == 0` as "report at every progress checkpoint", but
the benchmark uses `0` to skip installing `on_progress` entirely.

Use `--progress-counter-flush-interval N` to control how many terminal
executions each worker accumulates before flushing live progress counts into the
shared snapshot. `1` gives exact live counts; larger values reduce atomic
overhead but make progress counts more stale.
Use `--progress-poll-interval-steps N` to control how often workers poll the
clock for interval-throttled progress reporting. `0` and `1` both mean poll at
every progress checkpoint; larger values reduce progress overhead at the cost
of coarser callback timing.

For long-running runs where progress output is only occasional, these settings
are good starting points:

- About one update per minute with low overhead:

```bash
--progress-interval-ms 60000 \
--progress-poll-interval-steps 4096 \
--progress-counter-flush-interval 65536
```

This asks for at most one live callback per minute, only checks whether a
report is due every 4096 progress checkpoints, and flushes worker-local live
counts every 65536 terminal executions.

- About one update per 30 minutes with even lower overhead:

```bash
--progress-interval-ms 1800000 \
--progress-poll-interval-steps 16384 \
--progress-counter-flush-interval 262144
```

This makes callback timing and live counts coarser, but further reduces clock
polling and shared-counter flushing overhead.
