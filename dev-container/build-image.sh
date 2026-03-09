#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tag="dpor-dev"
no_cache=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-cache)
      no_cache="--no-cache"
      shift
      ;;
    -*)
      echo "Unknown option: $1" >&2
      echo "Usage: $0 [tag] [--no-cache]" >&2
      exit 2
      ;;
    *)
      tag="$1"
      shift
      ;;
  esac
done

uid="$(id -u)"
gid="$(id -g)"

DOCKER_BUILDKIT=1 docker build \
  ${no_cache} \
  --build-arg UID="${uid}" \
  --build-arg GID="${gid}" \
  -t "${tag}" \
  "${script_dir}"
