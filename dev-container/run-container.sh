#!/usr/bin/env bash
set -euo pipefail

tag="dpor-dev"
container_name=""
debug_mode=""
if [[ $# -gt 0 && "$1" != -* ]]; then
  tag="$1"
  shift
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --name)
      container_name="${2:-}"
      if [[ -z "${container_name}" ]]; then
        echo "Missing value for --name" >&2
        exit 2
      fi
      shift 2
      ;;
    --debug)
      debug_mode="debug"
      shift
      ;;
    --debug-full|--privileged)
      debug_mode="full"
      shift
      ;;
    --)
      shift
      break
      ;;
    *)
      break
      ;;
  esac
done

# Agent credentials — mount whichever are available.
claude_creds="${HOME}/.claude/.credentials.json"
codex_auth="${HOME}/.codex/auth.json"
has_claude=false
has_codex=false

if [[ -f "${claude_creds}" ]]; then
  has_claude=true
fi
if [[ -f "${codex_auth}" ]]; then
  has_codex=true
fi
if [[ "${has_claude}" == false && "${has_codex}" == false ]]; then
  echo "No agent credentials found. Provide at least one of:" >&2
  echo "  ${claude_creds}  (claude login)" >&2
  echo "  ${codex_auth}    (codex auth)" >&2
  exit 1
fi

project_name="${PROJECT_NAME:-$(basename "${PWD}")}"
if [[ -z "${container_name}" ]]; then
  container_name="$(printf 'dev-%s' "${project_name}" | tr '[:upper:]' '[:lower:]' | tr -c 'a-z0-9_.-' '-')"
fi

docker_args=(
  --rm -it
  --name "${container_name}"
  -e PROJECT_NAME="${project_name}"
  -v "${PWD}:/home/dev/project"

  # --- hardening (transparent to normal use) ---
  --cap-drop=ALL                        # drop all default capabilities
  --security-opt=no-new-privileges      # block setuid/setgid escalation
  --pids-limit=512                      # cap forked processes
  --memory=32g                           # cap memory usage
)

if [[ "${has_claude}" == true ]]; then
  docker_args+=(-v "${claude_creds}:/home/dev/.claude/.credentials.json:ro")
fi
if [[ "${has_codex}" == true ]]; then
  docker_args+=(-v "${codex_auth}:/home/dev/.codex/auth.json:ro")
fi

if [[ "${debug_mode}" == "debug" ]]; then
  # Re-add ptrace on top of the hardened baseline.
  docker_args+=(
    --cap-add=SYS_PTRACE
    --security-opt=seccomp=unconfined
    --security-opt=apparmor=unconfined
  )
elif [[ "${debug_mode}" == "full" ]]; then
  # Full privileges — replaces all hardening above.
  docker_args+=(
    --privileged
    --security-opt=seccomp=unconfined
    --security-opt=apparmor=unconfined
    --sysctl kernel.randomize_va_space=0
    --sysctl kernel.yama.ptrace_scope=0
  )
fi

docker run "${docker_args[@]}" "${tag}" "$@"
