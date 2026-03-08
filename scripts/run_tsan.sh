#!/usr/bin/env bash
# Build with the TSAN preset and run all tests, verifying TSAN-clean execution.
# Exits non-zero if any data race is detected.
#
# TSAN requires ASLR to be disabled. This script uses setarch to run the build
# and test steps with ASLR off (no root required). If ASLR is already disabled
# system-wide, setarch is still harmless.
#
# Usage:
#   scripts/run_tsan.sh                  # run all tests
#   scripts/run_tsan.sh [parallel]       # run only parallel-tagged tests
#   scripts/run_tsan.sh --repeat N       # repeat the test run N times
set -euo pipefail

filter=""
repeat=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    parallel)
      filter="parallel"
      shift
      ;;
    --repeat)
      if [[ $# -lt 2 ]]; then
        printf 'missing value for --repeat\n' >&2
        exit 2
      fi
      repeat="$2"
      shift 2
      ;;
    -h|--help)
      printf 'Usage: %s [parallel] [--repeat N]\n' "${0##*/}"
      exit 0
      ;;
    *)
      printf 'unknown argument: %s\n' "$1" >&2
      printf 'Usage: %s [parallel] [--repeat N]\n' "${0##*/}" >&2
      exit 2
      ;;
  esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
noaslr="setarch $(uname -m) -R"

printf '=== Configuring TSAN build ===\n'
cmake --preset tsan -S "${repo_root}"

# Build under setarch because catch_discover_tests runs TSAN-instrumented
# binaries at build time to enumerate test cases.
printf '=== Building (ASLR disabled for test discovery) ===\n'
${noaslr} cmake --build --preset tsan -j

# TSAN options: halt on first report so failures are clear.
export TSAN_OPTIONS="${TSAN_OPTIONS:-}${TSAN_OPTIONS:+ }halt_on_error=1"

ctest_args=(--preset tsan --output-on-failure)
if [[ -n "${filter}" ]]; then
  ctest_args+=(-R "${filter}")
fi

for i in $(seq 1 "${repeat}"); do
  if [[ "${repeat}" -gt 1 ]]; then
    printf '\n=== TSAN test run %d/%d ===\n' "${i}" "${repeat}"
  else
    printf '=== Running tests under TSAN ===\n'
  fi
  ${noaslr} ctest "${ctest_args[@]}"
done

printf '\nTSAN: all runs clean.\n'
