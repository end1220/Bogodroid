#!/bin/bash
# Build unityloader for the Anbernic H700 inside GlES_Dev.
#
#   bash /tmp/build-anbernic.sh <build-dir> <build-type>
#
# Sources must already be in /workspace/Bogodroid (GlES_Dev has no bind mount:
# use `docker cp <repo>/javastubs GlES_Dev:/workspace/Bogodroid/.` etc.).
#
# Build types used here:
#   Debug          -O0 -g, fastest to iterate, SLOW at runtime: never use it for
#                  a "the video is laggy" measurement.
#   RelWithDebInfo -O2 -g, keeps the log lines AND the symbols (fatal_error
#                  backtraces stay readable) while running at real speed.
set -e
BUILD_DIR="${1:-build-anbernic-rel}"
BUILD_TYPE="${2:-RelWithDebInfo}"

cd /workspace/Bogodroid
cmake -S . -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DUSE_MOLD=OFF \
  -DBD_FAKE_EGL=ON \
  -DBD_ENABLE_LOG=ON \
  -DBD_ENABLE_TRACE=OFF \
  -DBD_ENABLE_VERBOSE=OFF \
  -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF \
  -DBD_ENABLE_OPENSLES_SHIM=OFF
cmake --build "$BUILD_DIR" --target unityloader -j"$(nproc)"
ls -la "$BUILD_DIR/unityloader"
