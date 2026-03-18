#!/usr/bin/env bash
set -euo pipefail

tag="dpor-dev"
container_name=""
debug_mode=""
persist=""
mount_claude=""
allow_new_privs=""
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
    --persist)
      persist=1
      shift
      ;;
    --mount-claude-dir)
      mount_claude=1
      shift
      ;;
    --allow-new-privileges)
      allow_new_privs=1
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

codex_state_dir="${HOME}/.codex"
host_git_user_name="$(git config --global --get user.name 2>/dev/null || true)"
host_git_user_email="$(git config --global --get user.email 2>/dev/null || true)"
mkdir -p "${codex_state_dir}"

docker_args=(
  -it
  --init
  --name "${container_name}"
  -e PROJECT_NAME="${project_name}"
  -e COLORTERM=truecolor
  -v "${PWD}:/home/dev/project"
  -v "${codex_state_dir}:/home/dev/.codex"

  # --- hardening (transparent to normal use) ---
  --cap-drop=ALL                        # drop all default capabilities
  --security-opt=no-new-privileges      # block setuid/setgid escalation
  --pids-limit=512                      # cap forked processes
  --memory=32g                           # cap memory usage
)

if [[ -z "${persist}" ]]; then
  docker_args+=(--rm)
fi

if [[ -n "${mount_claude}" ]]; then
  claude_state_dir="${HOME}/.claude"
  mkdir -p "${claude_state_dir}"
  docker_args+=(
    -v "${claude_state_dir}:/home/dev/.claude"
    -e CLAUDE_CONFIG_DIR=/home/dev/.claude
  )
fi

if [[ -n "${host_git_user_name}" ]]; then
  docker_args+=(-e "HOST_GIT_USER_NAME=${host_git_user_name}")
fi

if [[ -n "${host_git_user_email}" ]]; then
  docker_args+=(-e "HOST_GIT_USER_EMAIL=${host_git_user_email}")
fi

git_init='if [[ -n "${HOST_GIT_USER_NAME:-}" ]]; then git config --global user.name "${HOST_GIT_USER_NAME}"; fi; if [[ -n "${HOST_GIT_USER_EMAIL:-}" ]]; then git config --global user.email "${HOST_GIT_USER_EMAIL}"; fi;'

if [[ "${debug_mode}" == "debug" ]]; then
  # Re-add ptrace on top of the hardened baseline.
  docker_args+=(
    --cap-add=SYS_PTRACE
    --security-opt=seccomp=unconfined
    --security-opt=apparmor=unconfined
  )
elif [[ "${debug_mode}" == "full" ]]; then
  # Full privileges — replaces all hardening above.
  allow_new_privs=1
  docker_args+=(
    --privileged
    --security-opt=seccomp=unconfined
    --security-opt=apparmor=unconfined
  )
fi

if [[ -n "${allow_new_privs}" ]]; then
  # Override the earlier no-new-privileges restriction.
  docker_args+=(--security-opt=no-new-privileges=false)
fi

if [[ "${debug_mode}" == "full" ]]; then
  # Set non-namespaced sysctls inside the container (Docker only
  # allows network-namespace sysctls via --sysctl).
  sysctl_init="sudo sysctl -w kernel.randomize_va_space=0 kernel.yama.ptrace_scope=0 >/dev/null;"
  if [[ $# -gt 0 ]]; then
    docker run "${docker_args[@]}" "${tag}" bash -lc "${sysctl_init} ${git_init} exec \"\$@\"" -- "$@"
  else
    docker run "${docker_args[@]}" "${tag}" bash -lc "${sysctl_init} ${git_init} exec bash -l"
  fi
else
  if [[ $# -gt 0 ]]; then
    docker run "${docker_args[@]}" "${tag}" bash -lc "${git_init} exec \"\$@\"" -- "$@"
  else
    docker run "${docker_args[@]}" "${tag}" bash -lc "${git_init} exec bash -l"
  fi
fi
