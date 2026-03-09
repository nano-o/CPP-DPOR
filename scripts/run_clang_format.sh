#!/usr/bin/env bash
# Run clang-format on project source files.
# Usage:
#   scripts/run_clang_format.sh              # Check changed files only (vs main)
#   scripts/run_clang_format.sh --fix        # Fix changed files in place
#   scripts/run_clang_format.sh --all        # Check all project files
#   scripts/run_clang_format.sh --all --fix  # Fix all project files in place

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

FIX=0
ALL=0
for arg in "$@"; do
  case "$arg" in
    --fix) FIX=1 ;;
    --all) ALL=1 ;;
    *) echo "Unknown option: $arg" >&2; exit 2 ;;
  esac
done

if [[ $ALL -eq 1 ]]; then
  mapfile -t FILES < <(
    find "$PROJECT_ROOT/include" "$PROJECT_ROOT/src" "$PROJECT_ROOT/tests" \
         "$PROJECT_ROOT/examples" "$PROJECT_ROOT/benchmarks" \
         -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.h' \) \
         2>/dev/null
  )
else
  # Only files changed relative to main (tracked source dirs only).
  MERGE_BASE=$(git -C "$PROJECT_ROOT" merge-base HEAD main 2>/dev/null || echo HEAD)
  mapfile -t FILES < <(
    git -C "$PROJECT_ROOT" diff --name-only --diff-filter=d "$MERGE_BASE" -- \
      'include/*.hpp' 'include/*.h' 'include/*.cpp' \
      'src/*.hpp' 'src/*.h' 'src/*.cpp' \
      'tests/*.hpp' 'tests/*.h' 'tests/*.cpp' \
      'examples/*.hpp' 'examples/*.h' 'examples/*.cpp' \
      'benchmarks/*.hpp' 'benchmarks/*.h' 'benchmarks/*.cpp' \
      2>/dev/null \
    | while read -r f; do echo "$PROJECT_ROOT/$f"; done
  )
fi

if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "No source files to check."
  exit 0
fi

if [[ $FIX -eq 1 ]]; then
  clang-format -i "${FILES[@]}"
  echo "Formatted ${#FILES[@]} files."
else
  DIFF=$(clang-format --dry-run --Werror "${FILES[@]}" 2>&1 || true)
  if [[ -n "$DIFF" ]]; then
    echo "clang-format check failed. Run with --fix to reformat."
    echo "$DIFF"
    exit 1
  fi
  echo "All ${#FILES[@]} files are correctly formatted."
fi
