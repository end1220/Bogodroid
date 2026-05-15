FROM debian:11

ENV DEBIAN_FRONTEND=noninteractive

# Enable arm64 multiarch — cross-compile builds link against :arm64 dev libs.
# Debian 11 (bullseye) ships glibc 2.31, so artifacts produced here run on
# any device with glibc >= 2.31 (Ubuntu 20.04+, Debian 11+, SteamOS, Raspberry
# Pi OS Bookworm, RHEL 9, and virtually all maintained distros).
RUN dpkg --add-architecture arm64 \
 && apt-get update -qq \
 && apt-get install -y -qq --no-install-recommends \
      cmake ninja-build pkg-config g++ \
      ca-certificates \
      crossbuild-essential-arm64 \
      libsdl2-dev libgles2-mesa-dev libegl1-mesa-dev \
      libcurl4-openssl-dev libsecret-1-dev \
      libbsd-dev libmd-dev \
      libsdl2-dev:arm64 libgles2-mesa-dev:arm64 libegl1-mesa-dev:arm64 \
      libcurl4-openssl-dev:arm64 libsecret-1-dev:arm64 \
      libbsd-dev:arm64 libmd-dev:arm64 \
 && apt-get clean && rm -rf /var/lib/apt/lists/*

WORKDIR /work
