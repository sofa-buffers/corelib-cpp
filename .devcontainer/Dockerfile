FROM ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive

# clang next to gcc: CI builds and runs the suite under both (the LTO leg
# especially -- their type-based alias analysis differs, and so does what LTO
# lets each conclude), so a devcontainer with only g++ cannot reproduce half
# the matrix.
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    clang \
    cmake \
    ninja-build \
    gdb \
    git \
    pkg-config \
    ca-certificates \
    curl \
    ripgrep \
    fd-find \
    shellcheck \
    shfmt \
    xxd \
    valgrind \
    linux-tools-generic \
    gcovr \
    jq \
    doxygen \
    graphviz \
    && rm -rf /var/lib/apt/lists/*

# GitHub CLI (gh)
RUN mkdir -p -m 755 /etc/apt/keyrings \
    && curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg \
        | tee /etc/apt/keyrings/githubcli-archive-keyring.gpg > /dev/null \
    && chmod go+r /etc/apt/keyrings/githubcli-archive-keyring.gpg \
    && echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" \
        > /etc/apt/sources.list.d/github-cli.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends gh \
    && rm -rf /var/lib/apt/lists/*

# Node.js (LTS) für Claude Code
RUN curl -fsSL https://deb.nodesource.com/setup_lts.x | bash - \
    && apt-get install -y --no-install-recommends nodejs \
    && rm -rf /var/lib/apt/lists/*

# Claude Code
RUN npm install -g @anthropic-ai/claude-code

WORKDIR /workspace

CMD ["/bin/bash"]
