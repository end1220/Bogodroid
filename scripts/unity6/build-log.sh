#!/bin/bash
# Build the diagnostic unityloader: same optimisation level as the shipping
# build (Release) but with BD logging compiled in, and WITHOUT stripping, so
# crash backtraces and [BD-*] lines are usable. ~90 MB, handheld only
# temporarily.
#
#   scripts/unity6/build-log.sh [TRACE]                          (host side)
#   bash /work/scripts/unity6/build-log.sh --in-container        (in the builder)
#
#   BUILD_DIR  cmake build dir (default build-unity6)
#   TRACE      any non-empty value also turns on BD_ENABLE_TRACE (BD_DEBUG /
#              BOOT_LOG); JNIVM_ENABLE_TRACE follows BD_ENABLE_LOG
#
# Never strip this one: `strip` removes exactly the symbols the build exists
# for, and the loss is silent until you need a backtrace.
# See docs/UNITY6.md 4 and AGENTS.md for the switch hierarchy.
set -eu

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
