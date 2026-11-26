# Linux release builds: Ubuntu 24.04 (GCC 13, glibc 2.39), so the client runs on 24.04 and newer (docs/ci.md).
# Pinned by digest, and apt reads the archive as it was at APT_SNAPSHOT (snapshot.ubuntu.com), so a rebuild gets the
# same packages. ca-certificates comes first from the image's own archive: the snapshot service is HTTPS only.
FROM ubuntu:24.04@sha256:562111b88caa79c71391c0c4d3c2114a7d5c35915c20561fd893cb1e07929e6b
ARG APT_SNAPSHOT=20261120T000000Z
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates \
    && echo "APT::Snapshot \"${APT_SNAPSHOT}\";" > /etc/apt/apt.conf.d/50snapshot && rm -rf /var/lib/apt/lists/*
# SDL finds Wayland only through pkg-config, needing egl.pc (libegl-dev) as well; without it SDL was built X11-only up
# to 0.7.0-beta, without a word (cmake/Dependencies.cmake now stops instead). pkg-config is listed rather than left to
# arrive with another package.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates python3 file ccache pkg-config \
    ninja-build glslang-tools zlib1g-dev clang libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
    libxfixes-dev libxss-dev libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev libdecor-0-dev libdrm-dev \
    libgbm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev && rm -rf /var/lib/apt/lists/*
# CEF's libcef.so links against these (GitHub's ubuntu-24.04 runners have them preinstalled).
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    libnss3 libnspr4 libatk1.0-0t64 libatk-bridge2.0-0t64 libcups2t64 libxcomposite1 libxdamage1 libcairo2 \
    libpango-1.0-0 libxrandr2 libxkbcommon0 libgbm1 libdrm2 libasound2t64 libdbus-1-3 libexpat1 libxshmfence1 \
    libglib2.0-0t64 libx11-xcb1 && rm -rf /var/lib/apt/lists/*
