#!/bin/bash
# ============================================================================
# DEPRECATED — 与 scripts/build-docker.sh 同属 x86 Debian 交叉路线，整条不通。
#
# 依赖 build-docker.sh 先配出 build-unity6/，而那一步在第一次编译就死在
# libavcodec/avcodec.h 找不到。仓库里从未存在过由这个脚本产出的 unityloader。
#
# 现行排查档做法（GlES_Dev 容器内，/workspace/Bogodroid，不 strip）：
#   cmake -S . -B build-anbernic-rel -G Ninja \
#     -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#     -DBD_ENABLE_LOG=ON -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF \
#     -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF &&
#   cmake --build build-anbernic-rel -j"$(nproc)" --target unityloader
# 完整命令见 docs/PORTING_PLAYBOOK.md §1.1 / §1.5。
# 要复活：先修镜像，再删掉下面那段 guard。
# ============================================================================
#
# 原始说明（历史）：Release 优化级 + LOG ON，不 strip（回溯可用，约 90 MB）。
#   scripts/unity6/build-log.sh [TRACE]                     (host side)
#   bash /work/scripts/unity6/build-log.sh --in-container   (in the builder)
#   BUILD_DIR  cmake build dir (default build-unity6)
#   TRACE      any non-empty value also turns on BD_ENABLE_TRACE
set -eu

cat >&2 <<'EOF'
[build-log.sh] DEPRECATED：依赖 build-docker.sh 那条从未跑通的 x86 交叉路线。
现行排查档在 GlES_Dev 容器内编 build-anbernic-rel（LOG=ON，不 strip），
见 docs/PORTING_PLAYBOOK.md §1.1 / §1.5。
EOF
exit 1

BUILD_DIR="${BUILD_DIR:-build-unity6}"
IMAGE="${BOGO_BUILDER_IMAGE:-bogo-builder:unity2017-armv7}"
PLATFORM="${BOGO_BUILDER_PLATFORM:-linux/amd64}"

if [ ! -d "$BUILD_DIR" ]; then
    echo "no $BUILD_DIR; configure once with scripts/build-docker.sh first" >&2
    exit 1
fi

if [ "${1:-}" != "--in-container" ]; then
    [ -n "${TRACE:-}" ] || TRACE="${1:-}"
    cd "$(dirname "$0")/../.."
    exec docker run --rm --platform "$PLATFORM" \
        -v "$PWD":/work -w "/work/$BUILD_DIR" \
        -e TRACE="$TRACE" \
        "$IMAGE" bash "/work/scripts/unity6/build-log.sh" --in-container
fi

# See build-rel.sh: a direct in-container call starts in /work, not the build dir.
if [ -n "${BUILD_DIR:-}" ] && [ -d "${BUILD_DIR}" ]; then
    cd "${BUILD_DIR}"
fi

if [ -n "${TRACE:-}" ]; then
    TRACE_FLAGS="-DBD_ENABLE_TRACE=ON"
else
    TRACE_FLAGS="-DBD_ENABLE_TRACE=OFF"
fi

cmake . -DCMAKE_BUILD_TYPE=Release \
    -DBD_ENABLE_LOG=ON $TRACE_FLAGS -DBD_ENABLE_VERBOSE=OFF \
    -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF > /tmp/cfg.log 2>&1 || {
    echo "configure failed:"; tail -30 /tmp/cfg.log; exit 1; }

cmake --build . -j"$(nproc)" --target unityloader 2>&1 | tail -3

echo "--- cache (LOG ON, RETURN_NON_ZERO OFF) ---"
grep -E 'JNIVM_ENABLE_RETURN_NON_ZERO|BD_ENABLE_LOG|BD_ENABLE_TRACE|CMAKE_BUILD_TYPE' CMakeCache.txt
echo "--- artifact (deliberately NOT stripped) ---"
ls -la unityloader
