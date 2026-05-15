FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq \
 && apt-get install -y -qq --no-install-recommends \
      cmake ninja-build g++ pkg-config mold \
      libsdl2-dev libgles2-mesa-dev libegl1-mesa-dev \
      libcurl4-openssl-dev libsecret-1-dev \
      libbsd-dev libmd-dev \
      ca-certificates \
 && apt-get clean \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work
