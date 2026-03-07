#!/usr/bin/env bash
set -euo pipefail

participants=4

while [[ $# -gt 0 ]]; do
  case "$1" in
    --participants)
      if [[ $# -lt 2 ]]; then
        printf 'missing value for --participants\n' >&2
        exit 2
      fi
      participants="$2"
      shift 2
      ;;
    *)
      printf 'usage: %s [--participants N]\n' "${0##*/}" >&2
      exit 2
      ;;
  esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
build_dir="${repo_root}/build/perf"
output_dir="${repo_root}/benchmarks/perf-data"
benchmark_rel="build/perf/benchmarks/two_phase_commit_timeout/dpor_two_phase_commit_timeout_benchmark"
benchmark_path="${repo_root}/${benchmark_rel}"
commit="$(git -C "${repo_root}" rev-parse --short HEAD)"
timestamp="$(date +%Y%m%d-%H%M%S)"
output_path="${output_dir}/perf-two-phase-commit-timeout-p${participants}-${commit}-${timestamp}.data"

cmake -S "${repo_root}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDPOR_BUILD_TESTING=ON \
  -DDPOR_BUILD_EXAMPLES=ON \
  -DDPOR_BUILD_BENCHMARKS=ON \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING="-O2 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls"

cmake --build "${build_dir}" --target dpor_two_phase_commit_timeout_benchmark -j

if [[ ! -x "${benchmark_path}" ]]; then
  printf 'benchmark executable not found at %s\n' "${benchmark_path}" >&2
  exit 1
fi

mkdir -p "${output_dir}"

cd "${repo_root}"

perf record -e cpu-clock --all-user -F 499 \
  --call-graph dwarf,4096 \
  -o "${output_path}" -- \
  "${benchmark_rel}" \
  --mode dpor --participants "${participants}" --iterations 1 --no-crash

printf 'Perf data written to %s\n' "${output_path}"
