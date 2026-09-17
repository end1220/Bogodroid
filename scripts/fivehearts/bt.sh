#!/bin/sh
# Symbolize unityloader addresses from a device backtrace.
#
#   sh /tmp/bt.sh 0x3c2434 0x3c2a78
#
# The loader and the game are both ELF with (at least) a dynamic symbol table,
# so module+offset pairs from BD-ABORT / fatal_error resolve here.
for a in "$@"; do
    echo "== $a (unityloader)"
    addr2line -f -C -i -e /tmp/ul.so "$a"
    echo "== $a (libunity)"
    addr2line -f -C -i -e /tmp/fh-libunity.so "$a" 2>/dev/null
done
