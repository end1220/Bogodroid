#!/usr/bin/env python3
"""
Patch pure-IL .NET assemblies that were built with <PlatformTarget>x64</PlatformTarget>
(PE machine = AMD64) so arm64 coreclr will load them. The IL is arch-neutral; only the
2-byte PE machine field is wrong. We flip AMD64 (0x8664) -> ARM64 (0xAA64).

ONLY touches assemblies that are pure IL (CLI ManagedNativeHeader == 0, i.e. NOT ReadyToRun /
crossgen images, which carry real x64 native code and must be rebuilt instead).

Without this, godot's `load_assembly_and_get_function_pointer(sts2.dll, ...)` fails silently
=> ".NET: Failed to get GodotPlugins initialization function pointer".

Usage:
    python3 patch_pe_machine.py <folder>        # scan *.dll, patch in place (.amd64.bak backups)
    python3 patch_pe_machine.py <folder> --dry  # report only
"""
import sys, os, glob, struct, shutil

def analyze(path):
    d = open(path, "rb").read()
    if d[:2] != b"MZ": return None
    try:
        e = struct.unpack_from("<I", d, 0x3C)[0]
        if d[e:e+4] != b"PE\x00\x00": return None
        coff = e + 4
        mach = struct.unpack_from("<H", d, coff)[0]
        optsz = struct.unpack_from("<H", d, coff+16)[0]
        opt = coff + 20
        magic = struct.unpack_from("<H", d, opt)[0]
        dd = opt + (112 if magic == 0x20B else 96)
        cli_rva = struct.unpack_from("<I", d, dd + 14*8)[0]
        if not cli_rva: return None
        nsec = struct.unpack_from("<H", d, coff+2)[0]
        so = opt + optsz
        def r2o(rva):
            for i in range(nsec):
                b = so + i*40
                va = struct.unpack_from("<I", d, b+12)[0]; vs = struct.unpack_from("<I", d, b+8)[0]
                praw = struct.unpack_from("<I", d, b+20)[0]; sraw = struct.unpack_from("<I", d, b+16)[0]
                if va <= rva < va + max(vs, sraw): return praw + (rva - va)
            return None
        c = r2o(cli_rva)
        mnh = struct.unpack_from("<I", d, c + 16 + 4 + 4 + 8*5)[0]  # ManagedNativeHeader RVA
        return {"machine": mach, "coff": coff, "r2r": mnh != 0}
    except Exception:
        return None

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    folder = sys.argv[1]; dry = "--dry" in sys.argv
    patched, skipped = [], []
    for f in sorted(glob.glob(os.path.join(folder, "*.dll"))):
        a = analyze(f)
        if not a or a["machine"] != 0x8664:
            continue
        if a["r2r"]:
            skipped.append(os.path.basename(f)); continue
        if dry:
            patched.append(os.path.basename(f) + " (would patch)"); continue
        shutil.copy(f, f + ".amd64.bak")
        d = bytearray(open(f, "rb").read())
        struct.pack_into("<H", d, a["coff"], 0xAA64)
        open(f, "wb").write(d)
        patched.append(os.path.basename(f))
    print("AMD64 pure-IL -> ARM64 (%d):" % len(patched))
    for p in patched: print("  ", p)
    if skipped:
        print("SKIPPED (ReadyToRun / has native x64 code, must rebuild):")
        for s in skipped: print("  ", s)

if __name__ == "__main__":
    main()
