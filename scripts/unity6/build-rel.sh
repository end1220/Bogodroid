#!/bin/bash
# Build the shippable unityloader: Release, BD logging off, stripped (~6 MB,
# vs ~90 MB with Debug symbols). This is what goes on the handhelds.
#
#   scripts/unity6/build-rel.sh            (host side; wraps docker)
#   bash /work/scripts/unity6/build-rel.sh --in-container   (inside the builder)
#
#   BUILD_DIR  cmake build dir (default build-unity6)
#   SYNC_TO    optional path to copy the stripped binary to
#
# Run from the repo root on the host; it re-execs itself in the builder image
# the same way scripts/build-docker.sh does. Passes
# -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF explicitly: it is a CMake cache entry, and
# a leftover ON from an experiment makes unityloader abort (exit 134) in code
# that looks unrelated. See AGENTS.md and docs/PORTING_PLAYBOOK.md 1.2.
set -eu

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
