# dev-container

Sandboxed development container for running AI coding agents (Claude Code,
Codex) against this project.  The container has the full C++20 toolchain
pre-installed so agents can build, test, and iterate without access to the
host system.

## Why a container?

Coding agents run arbitrary shell commands.  A container limits the blast
radius: the agent can only see the mounted project directory plus the
bind-mounted agent state directories (`~/.codex` by default; `~/.claude`
only with `--mount-claude-dir`).  Everything else on the host is invisible.
Damage is confined to those mounts: Git can restore tracked project files,
but untracked files and mounted agent state need their own backups.

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
| Static analysis | `cppcheck`, `iwyu` (include-what-you-use) |
| Build system | `cmake`, `ninja-build` |
| Test framework | `catch2` |
| AI agents | Claude Code (native installer, `claude.ai/install.sh`), Codex CLI (`@openai/codex` via npm) |
| Shell / editor | `bash`, `tmux`, `vim`, `fzf`, `ripgrep`, `fd-find`, `bat`, `jq` |
| Networking | `curl`, `wget`, `openssh-client` |

The image runs as a non-root `dev` user (UID/GID matched to the host). The
sudoers file grants passwordless `sudo`, but the default hardened launch
blocks it (see [Hardening](#hardening)); pass `--allow-new-privileges` to
install additional packages on the fly.

## Git identity

On startup, `run-container.sh` reads `user.name` and `user.email` from your
host global Git config and applies those values inside the container with
`git config --global`. Only those two fields are imported; signing settings
and other host Git config are not mounted into the container.

## Agent login

Agent CLIs are pre-installed but not pre-authenticated. The launcher
bind-mounts your host `~/.codex` directory into the container, so the first
Codex login persists across later runs. `~/.claude` is mounted only when you
pass `--mount-claude-dir`; without it, a Claude Code login lasts only for the
lifetime of that container. Log in inside the container:

```bash
claude auth login    # Claude Code (persists across runs with --mount-claude-dir)
codex login          # Codex
```

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

**Note:** under the default hardened baseline, `sudo` does **not** work:
`no-new-privileges` prevents setuid binaries (including `sudo`) from
elevating at all, even though the sudoers file grants passwordless access —
and with `--cap-drop=ALL`, even an elevated root process would lack the
capabilities to change uid/gid. To use `sudo` (e.g. for `apt install`),
relaunch with `--allow-new-privileges`, which sets `no-new-privileges=false`
and re-adds the capabilities `sudo` and `apt`/`dpkg` need (`SETUID`,
`SETGID`, `AUDIT_WRITE`, `CHOWN`, `DAC_OVERRIDE`, `FOWNER`, `FSETID`) while
keeping the remaining hardening measures. `--debug-full` implies it.

## Options

```
dev-container/run-container.sh [tag] [options] [-- command...]
```

| Option | Description |
|---|---|
| `tag` | Docker image tag (default: `dpor-dev`) |
| `--name NAME` | Custom container name (default: `dev-<project>`) |
| `--debug` | Add `SYS_PTRACE` and disable seccomp/apparmor |
| `--debug-full` | Privileged mode with ASLR and ptrace scope disabled (alias: `--privileged`; implies `--allow-new-privileges`) |
| `--persist` | Keep the container after exit (default runs with `--rm`) |
| `--mount-claude-dir` | Bind-mount host `~/.claude` so Claude Code logins persist |
| `--allow-new-privileges` | Re-enable `sudo` (e.g. for `apt install`): sets `no-new-privileges=false` and re-adds the capabilities it needs |
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
| `.claude/settings.local.json` | Claude Code permission presets for working in this directory |
