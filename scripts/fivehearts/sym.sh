#!/bin/bash
# Symbolize module-relative offsets inside the game's libunity.so.
# Usage: sym.sh <offset> [module-path]
L=${2:-/tmp/fh-libunity.so}
OFF=${1:?offset}
printf 'module: %s\n' "$L"
readelf -S "$L" | grep -E 'symtab|dynsym|debug' || true
echo "--- addr2line $OFF ---"
addr2line -f -C -i -e "$L" "$OFF" || true
echo "--- nearest preceding dynamic symbol ---"
nm -D --defined-only "$L" | sort > /tmp/s.txt
wc -l < /tmp/s.txt
python3 "${NEAREST:-/tmp/nearest.py}" /tmp/s.txt "$OFF"
