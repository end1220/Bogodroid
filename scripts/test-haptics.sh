#!/usr/bin/env bash
# Run in the Linux build container; production code, simulated SDL/evdev only.
set -eu
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -pthread \
  -Iplatform/common scripts/test-haptics.cpp platform/common/haptics_backend.cpp \
  -Wl,--wrap=dlsym,--wrap=opendir,--wrap=readdir,--wrap=closedir \
  -Wl,--wrap=open,--wrap=close,--wrap=ioctl -ldl -o "$out/test-haptics"
"$out/test-haptics"
"$out/test-haptics" legacy

# TrimUI motor path: simulated sysfs attributes, no pad at all.
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -pthread \
  -Iplatform/common scripts/test-haptics-motor.cpp platform/common/haptics_backend.cpp \
  -Wl,--wrap=open,--wrap=close,--wrap=lseek,--wrap=write -ldl -o "$out/test-haptics-motor"
"$out/test-haptics-motor"
