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

project_name="${PROJECT_NAME:-$(basename "${PWD}")}"
if [[ -z "${container_name}" ]]; then
  container_name="$(printf 'dev-%s' "${project_name}" | tr '[:upper:]' '[:lower:]' | tr -c 'a-z0-9_.-' '-')"
fi

claude_state_dir="${HOME}/.claude"
codex_state_dir="${HOME}/.codex"
mkdir -p "${claude_state_dir}" "${codex_state_dir}"

docker_args=(
  --rm -it
  --name "${container_name}"
  -e PROJECT_NAME="${project_name}"
  -e COLORTERM=truecolor
  -v "${PWD}:/home/dev/project"
  -v "${claude_state_dir}:/home/dev/.claude"
  -v "${codex_state_dir}:/home/dev/.codex"

  # --- hardening (transparent to normal use) ---
  --cap-drop=ALL                        # drop all default capabilities
  --security-opt=no-new-privileges      # block setuid/setgid escalation
  --pids-limit=512                      # cap forked processes
  --memory=32g                           # cap memory usage
)

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
  )
fi

if [[ "${debug_mode}" == "full" ]]; then
  # Set non-namespaced sysctls inside the container (Docker only
  # allows network-namespace sysctls via --sysctl).
  sysctl_init="sudo sysctl -w kernel.randomize_va_space=0 kernel.yama.ptrace_scope=0 >/dev/null;"
  if [[ $# -gt 0 ]]; then
    docker run "${docker_args[@]}" "${tag}" bash -c "${sysctl_init} exec \"\$@\"" -- "$@"
  else
    docker run "${docker_args[@]}" "${tag}" bash -c "${sysctl_init} exec bash -l"
  fi
else
  docker run "${docker_args[@]}" "${tag}" "$@"
fi
