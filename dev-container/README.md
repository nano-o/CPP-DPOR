# dev-container

Sandboxed development container for running AI coding agents (Claude Code,
Codex) against this project.  The container has the full C++20 toolchain
pre-installed so agents can build, test, and iterate without access to the
host system.

## Why a container?

Coding agents run arbitrary shell commands.  A container limits the blast
radius: the agent can only see the mounted project directory and read-only
credential files.  Everything else on the host is invisible.  If an agent
does something destructive, `git checkout` restores the working tree —
nothing outside the mount is affected.

## Quick start

```bash
# 1. Build the image (once, or after Dockerfile changes)
dev-container/build-image.sh

# 2. Run from the project root — mounts $PWD into the container
dev-container/run-container.sh
```

Inside the container you land in `/home/dev/project` (the mounted repo) and
can immediately build:

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

## What's in the image

| Category | Packages |
|---|---|
| C++ toolchain | `g++`, `clang`, `clang-format`, `clang-tidy`, `gdb` |
| Build system | `cmake`, `ninja-build` |
| Test framework | `catch2` |
| AI agents | Codex CLI (`@openai/codex`); Claude Code can be installed at runtime via `npm i -g @anthropic-ai/claude-code` |
| Shell / editor | `bash`, `tmux`, `vim`, `fzf`, `ripgrep`, `fd-find`, `bat`, `jq` |
| Networking | `curl`, `wget`, `openssh-client` |

The image runs as a non-root `dev` user (UID/GID matched to the host) with
passwordless `sudo` for installing additional packages on the fly.

## Credential handling

`run-container.sh` auto-detects which agent credentials exist on the host
and bind-mounts them read-only:

| Agent | Host path | Container path |
|---|---|---|
| Claude Code | `~/.claude/.credentials.json` | `/home/dev/.claude/.credentials.json` |
| Codex | `~/.codex/auth.json` | `/home/dev/.codex/auth.json` |

At least one must be present.  If both exist, both are mounted so you can
use either agent inside the same container.

## Debug modes

For running GDB or sanitizers (ASAN/TSAN) inside the container, additional
Linux capabilities are needed:

```bash
# ptrace support (gdb, strace, sanitizers)
dev-container/run-container.sh --debug

# full privileged mode (ASLR disabled, unrestricted ptrace)
dev-container/run-container.sh --debug-full
```

The default (no flag) runs with the hardened baseline described below.

## Hardening

The default container runs with several security measures beyond standard
Docker isolation, all transparent to normal agent workflows:

| Measure | Flag | Effect |
|---|---|---|
| Drop capabilities | `--cap-drop=ALL` | Removes all default Linux capabilities (NET_RAW, CHOWN, SETUID, etc.) — agents have no use for them |
| No new privileges | `--security-opt=no-new-privileges` | Blocks setuid/setgid escalation inside the container |
| PID limit | `--pids-limit=512` | Prevents fork bombs from a runaway agent |
| Memory limit | `--memory=32g` | Caps memory usage so a runaway build can't OOM the host |

These are layered on top of standard Docker isolation (namespaces,
cgroups, default seccomp profile).  The `--debug` and `--debug-full`
flags selectively relax these for debugging use cases.

**Note:** `sudo` inside the container still works for `apt install` etc.
(the sudoers file grants it) but `no-new-privileges` prevents setuid
binaries from gaining elevated kernel capabilities — the combination is
safe because the container already has `ALL` capabilities dropped.

## Options

```
dev-container/run-container.sh [tag] [options] [-- command...]
```

| Option | Description |
|---|---|
| `tag` | Docker image tag (default: `dpor-dev`) |
| `--name NAME` | Custom container name (default: `dev-<project>`) |
| `--debug` | Add `SYS_PTRACE` and disable seccomp/apparmor |
| `--debug-full` | Privileged mode with ASLR and ptrace scope disabled |
| `-- command...` | Override the default shell (e.g. `-- bash -c "cmake --preset debug"`) |

## Rebuilding

```bash
# Normal rebuild (uses Docker layer cache)
dev-container/build-image.sh

# Force full rebuild
dev-container/build-image.sh --no-cache
```

## Files

| File | Purpose |
|---|---|
| `Dockerfile` | Image definition |
| `build-image.sh` | Build the image with host UID/GID |
| `run-container.sh` | Launch a container with project + credentials mounted |
| `tmux.conf` | tmux config (vim keys, OSC52 clipboard, 256-color) |
| `osc52-tmux` | Clipboard helper for tmux over SSH/containers |
| `project-title.sh` | Sets terminal title to the project name |
