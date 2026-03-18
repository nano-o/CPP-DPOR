# Benchmarks

This directory contains standalone benchmark executables for the two-phase
commit examples. They do not use Catch2 and can be built with
`DPOR_BUILD_TESTING=OFF`, so their timings reflect the DPOR/oracle work rather
than test harness overhead.

## Available benchmarks

- `dpor_two_phase_commit_timeout_benchmark`
  2PC model with timers, using the explicit scenario headers
  `examples/two_phase_commit_timeout/sim/nominal.hpp` and
  `examples/two_phase_commit_timeout/sim/crash_before_decision.hpp`.

The executable supports the following CLI:

```text
--mode dpor|oracle|both
--participants N
--iterations N
--no-crash
--parallel
--max-workers N
--max-queued-tasks N
--spawn-depth-cutoff N
--min-fanout N
```

- `--mode dpor` measures only DPOR exploration.
- `--mode oracle` measures only the exhaustive oracle.
- `--mode both` runs both and checks that they agree on the execution count.
- `--participants` defaults to `3`.
- `--iterations` defaults to `1`.
- `--no-crash` disables the coordinator crash choice so you can benchmark the
  no-crash state space separately.
- `--parallel` switches DPOR runs from `verify()` to `verify_parallel()`. If
  `--max-workers` is omitted, worker count falls back to the library's
  hardware-based default.
- `--max-workers`, `--max-queued-tasks`, `--spawn-depth-cutoff`, and
  `--min-fanout` pass through to `ParallelVerifyOptions`. Supplying any of
  these also enables `--parallel`.
- Parallel flags are ignored in `--mode oracle`.

When the oracle runs, the benchmark also prints `paths_explored`, which counts
raw oracle DFS terminal paths before deduplication. That number is typically
much larger than the final `executions` count.

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
Plain 2PC benchmark participants=2 inject_crash=true iterations=1 optimized_build=true
DPOR
  run 1: executions=24 elapsed_ms=...
  summary: min_ms=... avg_ms=... max_ms=... executions=24
Oracle
  run 1: executions=24 paths_explored=... elapsed_ms=...
  summary: min_ms=... avg_ms=... max_ms=... executions=24 paths_explored=...
```

Use `--iterations N` to get repeated timings in a single process.
Parallel runs print the selected `ParallelVerifyOptions` on the header line.
