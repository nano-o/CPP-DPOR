FROM ubuntu:24.04

# Build-time knobs for user and image behavior.
ARG DEBIAN_FRONTEND=noninteractive
ARG USER=dev
ARG UID=1000
ARG GID=1000

# Locale defaults for consistent tooling behavior.
ENV LANG=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8

# Base tooling + C++20 build dependencies.
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        bat \
        cmake \
        curl \
        fd-find \
        fzf \
        g++ \
        gdb \
        git \
        jq \
        less \
        locales \
        lsof \
        ncurses-term \
        ninja-build \
        nodejs \
        npm \
        openssh-client \
        procps \
        ripgrep \
        bash-completion \
        bubblewrap \
        build-essential \
        clang \
        clang-format \
        clang-tidy \
        cppcheck \
        iwyu \
        poppler-utils \
        python3 \
        python3-pip \
        python3-venv \
        socat \
        sudo \
        tmux \
        vim \
        wget; \
    locale-gen en_US.UTF-8; \
    rm -rf /var/lib/apt/lists/*

# Install Catch2 v3 from source (the Ubuntu package lacks CMake config files).
RUN set -eux; \
    git clone --depth 1 --branch v3.8.0 https://github.com/catchorg/Catch2.git /tmp/catch2; \
    cmake -S /tmp/catch2 -B /tmp/catch2/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCATCH_INSTALL_DOCS=OFF \
        -DCATCH_INSTALL_EXTRAS=ON; \
    cmake --build /tmp/catch2/build; \
    cmake --install /tmp/catch2/build; \
    rm -rf /tmp/catch2

# Create or reconcile the dev user/group with desired UID/GID.
RUN set -eux; \
    existing_group="$(getent group "${GID}" | cut -d: -f1 || true)"; \
    if [ -z "${existing_group}" ]; then \
        if getent group "${USER}" >/dev/null; then \
            groupmod --gid "${GID}" "${USER}"; \
        else \
            groupadd --gid "${GID}" "${USER}"; \
        fi; \
    fi; \
    existing_user="$(getent passwd "${UID}" | cut -d: -f1 || true)"; \
    if id -u "${USER}" >/dev/null 2>&1; then \
        usermod --uid "${UID}" --gid "${GID}" -s /bin/bash "${USER}"; \
    elif [ -n "${existing_user}" ]; then \
        usermod --login "${USER}" --home "/home/${USER}" --move-home "${existing_user}"; \
        usermod --uid "${UID}" --gid "${GID}" -s /bin/bash "${USER}"; \
    else \
        useradd --uid "${UID}" --gid "${GID}" -m -s /bin/bash "${USER}"; \
    fi; \
    echo "${USER} ALL=(ALL) NOPASSWD:ALL" > "/etc/sudoers.d/${USER}"; \
    chmod 0440 "/etc/sudoers.d/${USER}"

# Prepare user-writable XDG and npm directories.
RUN install -d -o "${UID}" -g "${GID}" \
    "/home/${USER}/.local/bin" \
    "/home/${USER}/.local/share" \
    "/home/${USER}/.local/state" \
    "/home/${USER}/.cache" \
    "/home/${USER}/.config" \
    "/home/${USER}/.npm-global"

# npm global prefix for the dev user.
ENV NPM_CONFIG_PREFIX="/home/${USER}/.npm-global" \
    PATH="/home/${USER}/.local/bin:/home/${USER}/.npm-global/bin:${PATH}"

# Install Claude Code via native installer (npm method is deprecated).
RUN su - "${USER}" -c "curl -fsSL https://claude.ai/install.sh | bash"

# Install Codex CLI via npm.
RUN su - "${USER}" -c "NPM_CONFIG_PREFIX=/home/${USER}/.npm-global npm install -g @openai/codex && npm cache clean --force"

# Create mount points and user config.
RUN install -d -o "${UID}" -g "${GID}" "/home/${USER}/project" "/home/${USER}/.claude" "/home/${USER}/.codex"
COPY --chown=${UID}:${GID} tmux.conf /home/${USER}/.tmux.conf
COPY --chown=${UID}:${GID} osc52-tmux /home/${USER}/.local/bin/osc52-tmux
RUN chmod 0755 /home/${USER}/.local/bin/osc52-tmux
COPY project-title.sh /etc/profile.d/project-title.sh
RUN grep -qxF 'source /etc/profile.d/project-title.sh' /etc/bash.bashrc \
    || echo 'source /etc/profile.d/project-title.sh' >> /etc/bash.bashrc
RUN if [ -f "/home/${USER}/.bashrc" ]; then \
        grep -qxF 'source /etc/profile.d/project-title.sh' "/home/${USER}/.bashrc" \
            || echo 'source /etc/profile.d/project-title.sh' >> "/home/${USER}/.bashrc"; \
    else \
        cp /etc/skel/.bashrc "/home/${USER}/.bashrc"; \
        echo 'source /etc/profile.d/project-title.sh' >> "/home/${USER}/.bashrc"; \
        chown "${UID}:${GID}" "/home/${USER}/.bashrc"; \
    fi

# Default to the dev user and mounted project directory.
USER ${USER}
WORKDIR /home/${USER}/project
