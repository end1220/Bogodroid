#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
#
# ============================================================================
# DEPRECATED — 这条路线从未跑通，不要用。理由见 BUILD-DOCKER.md §5。
#
# 死在 configure 之后的第一次编译：
#   FAILED: thunks/ndk/media.cpp.o
#   fatal error: libavcodec/avcodec.h: No such file or directory
#
# 原因：Dockerfile.builder 基于 debian:11，没装 FFmpeg dev 头，而
# thunks/ndk/media.cpp、thunks/ndk/ffmpeg_optional.cpp、
# projects/unityloader/startup_video.cpp 都无条件 #include <libavcodec/avcodec.h>，
# CMake 那边只有 pkg_check_modules(FFMPEG QUIET) —— 找不到也只打一行 STATUS。
# 补装同样不行：Debian 11 已 EOL，ffmpeg 包在源里 404；换归档源会让 apt 为了
# 解依赖把 cmake 卸掉。
#
# 现行做法：在 GlES_Dev 容器内（/workspace/Bogodroid）直接编。
#   发版：cmake -S . -B build-device-release ... + aarch64-linux-gnu-strip
#   排查：同一条命令换 -DBD_ENABLE_LOG=ON，不 strip，用另一个目录
# 完整命令见 docs/PORTING_PLAYBOOK.md §1.1 与 BUILD-DOCKER.md §2。
#
# 要复活这个脚本：先把镜像补成带 FFmpeg dev 头的完整工具链，再删掉下面那段
# guard。
# ============================================================================
#
# 原始说明（历史，保留备查）：交叉编译 unityloader，
# Debian 11 → aarch64-linux-gnu-g++ → glibc 2.31。
#   BOGO_BUILDER_IMAGE / BOGO_BUILDER_PLATFORM / BUILD_DIR / REBUILD_IMAGE

set -euo pipefail

cd "$(dirname "$0")/.."

cat >&2 <<'EOF'
[build-docker.sh] DEPRECATED：x86 Debian 交叉路线从未跑通
  （镜像缺 FFmpeg dev 头；Debian 11 源 404，补装会拆掉 cmake）。
现行构建在 GlES_Dev 容器内进行：
  docs/PORTING_PLAYBOOK.md §1.1（完整命令）
  BUILD-DOCKER.md §2（容器基线、源码同步、产物检查）
EOF
exit 1

IMAGE="${BOGO_BUILDER_IMAGE:-bogo-builder:dev}"
BUILD_DIR="${BUILD_DIR:-build-docker}"
BUILD_TYPE="${1:-Release}"

PLATFORM="${BOGO_BUILDER_PLATFORM:-}"
if [ -z "$PLATFORM" ]; then
  case "$(uname -m)" in
    x86_64|amd64)  PLATFORM=linux/amd64 ;;
    arm64|aarch64) PLATFORM=linux/arm64 ;;
    *) echo "unsupported host arch: $(uname -m)"; exit 1 ;;
  esac
fi

if [ "${REBUILD_IMAGE:-0}" = "1" ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo ">>> building $IMAGE ($PLATFORM)..."
  docker build --platform="$PLATFORM" \
    -f Dockerfile.builder -t "$IMAGE" .
fi

echo ">>> compiling unityloader → $BUILD_DIR/unityloader  ($BUILD_TYPE, $PLATFORM)"
docker run --rm --platform="$PLATFORM" \
  -v "$PWD":/work -w /work "$IMAGE" bash -c "
    set -e
    cmake -B '$BUILD_DIR' -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake \
      -DCMAKE_BUILD_TYPE='$BUILD_TYPE'
    cmake --build '$BUILD_DIR' -j
  "

echo
echo ">>> result:"
ls -la "$BUILD_DIR/unityloader"

echo
echo ">>> sanity check:"
docker run --rm --platform="$PLATFORM" \
  -v "$PWD":/work -w /work "$IMAGE" bash -c "
    echo '--- DT_NEEDED ---'
    objdump -p '$BUILD_DIR/unityloader' | grep NEEDED
    echo
    echo '--- max GLIBC symbol ---'
    LC_ALL=C grep -ao 'GLIBC_[0-9.]*' '$BUILD_DIR/unityloader' | sort -uV | tail -3
  "
