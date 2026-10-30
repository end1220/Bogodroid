#!/bin/bash
# ============================================================================
# DEPRECATED — 与 scripts/build-docker.sh 同属 x86 Debian 交叉路线，整条不通。
#
# 这个脚本要求先用 build-docker.sh 配出 build-unity6/，而那一步的第一次编译
# 就死在 libavcodec/avcodec.h 找不到（镜像缺 FFmpeg dev 头，Debian 11 源已 404）。
# 也就是说：仓库里从未存在过由这个脚本产出的 unityloader。
#
# 现行发版做法（GlES_Dev 容器内，/workspace/Bogodroid）：
#   cmake -S . -B build-device-release -G Ninja \
#     -DCMAKE_BUILD_TYPE=Release \
#     -DBD_ENABLE_LOG=OFF -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF \
#     -DIL2CPP_TRACE=OFF -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF &&
#   cmake --build build-device-release -j"$(nproc)" --target unityloader &&
#   aarch64-linux-gnu-strip build-device-release/unityloader
# 完整命令见 docs/PORTING_PLAYBOOK.md §1.1 与 BUILD-DOCKER.md §2。
# 要复活：先修镜像，再删掉下面那段 guard。
# ============================================================================
#
# 原始说明（历史）：Release + LOG OFF + strip（~6 MB，Debug 档约 90 MB）。
#   scripts/unity6/build-rel.sh                            (host side)
#   bash /work/scripts/unity6/build-rel.sh --in-container  (in the builder)
#   BUILD_DIR  cmake build dir (default build-unity6)
#   SYNC_TO    optional path to copy the stripped binary to
set -eu

cat >&2 <<'EOF'
[build-rel.sh] DEPRECATED：依赖 build-docker.sh 那条从未跑通的 x86 交叉路线。
现行发版在 GlES_Dev 容器内 cmake -S . -B build-device-release 加 strip，
见 docs/PORTING_PLAYBOOK.md §1.1 / BUILD-DOCKER.md §2。
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
    cd "$(dirname "$0")/../.."
    exec docker run --rm --platform "$PLATFORM" \
        -v "$PWD":/work -w "/work/$BUILD_DIR" \
        -e SYNC_TO="${SYNC_TO:-}" \
        "$IMAGE" bash "/work/scripts/unity6/build-rel.sh" --in-container
fi

# `cmake .` needs to run in the build dir. Via the wrapper that is already the
# cwd (-w), but a direct in-container call starts in /work, where configuring
# would sprinkle CMakeCache.txt/Makefile/CMakeFiles over the repo root.
if [ -n "${BUILD_DIR:-}" ] && [ -d "${BUILD_DIR}" ]; then
    cd "${BUILD_DIR}"
fi

cmake . -DCMAKE_BUILD_TYPE=Release \
    -DBD_ENABLE_LOG=OFF -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF \
    -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF > /tmp/cfg.log 2>&1 || {
    echo "configure failed:"; tail -30 /tmp/cfg.log; exit 1; }

cmake --build . -j"$(nproc)" --target unityloader 2>&1 | tail -3

echo "--- cache (must match: LOG/TRACE OFF, RETURN_NON_ZERO OFF) ---"
grep -E 'JNIVM_ENABLE_RETURN_NON_ZERO|JNIVM_ENABLE_DEBUG|BD_ENABLE_LOG|BD_ENABLE_TRACE|CMAKE_BUILD_TYPE' CMakeCache.txt

aarch64-linux-gnu-strip --strip-unneeded unityloader
echo "--- artifact ---"
ls -la unityloader
file unityloader

# Optional: copy the shippable binary somewhere the host can pick it up, e.g.
#   SYNC_TO=/work/gamefiles/unity6/unityloader scripts/unity6/build-rel.sh
if [ -n "${SYNC_TO:-}" ]; then
    cp -f unityloader "$SYNC_TO"
    echo "copied to $SYNC_TO"
fi
