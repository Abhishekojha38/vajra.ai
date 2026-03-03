# SPDX-License-Identifier: MIT
# Aham AI Agent — Multi-stage Docker build
#
# Build:  docker build -t aham .
# Run:    docker run -it --rm -e MODEL=mistral-nemo:latest aham

# Stage 1: Build
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    libcurl4-openssl-dev \
    libreadline-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

RUN mkdir -p build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make -j"$(nproc)"

# Stage 2: Runtime
FROM ubuntu:24.04

# Network & serial expert tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    # Runtime libs
    ca-certificates \
    libcurl4 \
    libreadline8 \
    # Shell utilities
    bash \
    coreutils \
    procps \
    # Networking tools (network + wireless skills)
    iproute2 \
    iputils-ping \
    dnsutils \
    tcpdump \
    tmux \
    socat \
    openssh-client \
    rsync \
    # System diagnosis (linux skill)
    sysstat \
    lsof \
    strace \
    smartmontools \
    mailutils \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Binary
COPY --from=builder /build/build/bin/aham .

# Config & security
COPY aham.conf allowlist.conf .env ./

# Static assets (web UI)
COPY assets/ ./assets/

# Skills (workspace-level; seeded into workspace on first run)
COPY skills/ ./skills/

# Helper scripts (tmux wait/find utilities)
RUN chmod +x ./skills/tmux/scripts/*.sh

# Templates (seeded into workspace on first run; never overwrite)
COPY templates/ ./templates/

# Writable workspace volumes (memory persists across container restarts)
VOLUME ["/app/memory", "/app/skills"]

# Create workspace directories
RUN mkdir -p memory

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD pgrep aham > /dev/null || exit 1

ENTRYPOINT ["./aham"]
CMD []
