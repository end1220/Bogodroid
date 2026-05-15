#!/usr/bin/env bash
# Cross-compile unityloader inside the Dockerfile.builder image
# (Debian 11 → aarch64-linux-gnu-g++ → glibc 2.31 target).
#
# Usage:
#   scripts/build-docker.sh                  # Release into build-docker/
#   scripts/build-docker.sh Debug            # Debug into build-docker/
#   BUILD_DIR=build-foo scripts/build-docker.sh
#
# Env overrides:
#   BOGO_BUILDER_IMAGE     builder image tag    (default: bogo-builder:dev)
#   BOGO_BUILDER_PLATFORM  docker --platform    (auto-detected from host arch)
#   BUILD_DIR              cmake build dir      (default: build-docker)
#   REBUILD_IMAGE=1        force rebuild of the builder image

set -euo pipefail

cd "$(dirname "$0")/.."

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
