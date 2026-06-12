#!/usr/bin/env python3
# Minimal ECMA-335 metadata reader: confirm a type+method exists in a .NET assembly.
import struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else "/Users/smallraw/Downloads/Slay the Spire 2/data_sts2_windows_x86_64/sts2.dll"
d = open(path, "rb").read()

def u8(o):  return d[o]
def u16(o): return struct.unpack_from("<H", d, o)[0]
def u32(o): return struct.unpack_from("<I", d, o)[0]
def u64(o): return struct.unpack_from("<Q", d, o)[0]

# --- PE ---
e_lfanew = u32(0x3C)
assert d[e_lfanew:e_lfanew+4] == b"PE\x00\x00", "not PE"
coff = e_lfanew + 4
num_sec = u16(coff + 2)
opt_size = u16(coff + 16)
opt = coff + 20
magic = u16(opt)
if magic == 0x20B:   # PE32+
    nrva_off = opt + 108; dd = opt + 112
else:                # PE32
    nrva_off = opt + 92;  dd = opt + 96
# CLI header = data directory index 14
cli_rva = u32(dd + 14*8)
# sections
secs = []
so = opt + opt_size
for i in range(num_sec):
    b = so + i*40
    va = u32(b+12); vs = u32(b+8); praw = u32(b+20); sraw = u32(b+16)
    secs.append((va, vs, praw, sraw))
def rva2off(rva):
    for va, vs, praw, sraw in secs:
        if va <= rva < va + max(vs, sraw):
            return praw + (rva - va)
    raise ValueError("rva %x not mapped" % rva)

cli = rva2off(cli_rva)
meta_rva = u32(cli + 8)
meta = rva2off(meta_rva)
assert u32(meta) == 0x424A5342, "bad metadata sig"
ver_len = u32(meta + 12)
p = meta + 16 + ((ver_len + 3) & ~3)
flags = u16(p); nstreams = u16(p+2)
p += 4
streams = {}
for i in range(nstreams):
    off = u32(p); size = u32(p+4)
    n = p + 8
    name = b""
    while d[n] != 0:
        name += d[n:n+1]; n += 1
    streams[name.decode()] = (meta + off, size)
    n += 1
    p = (n + 3) & ~3  # pad to 4 from start of name? spec: padded to 4-byte boundary
    # name length incl null, padded to multiple of 4
    # recompute properly:
    # (handled below)
# redo stream parse robustly
streams = {}
p = meta + 16 + ((ver_len + 3) & ~3) + 4
for i in range(nstreams):
    off = u32(p); size = u32(p+4)
    n = p + 8; start = n
    while d[n] != 0: n += 1
    name = d[start:n].decode()
    streams[name] = (meta + off, size)
    namelen = (n - start) + 1
    namelen = (namelen + 3) & ~3
    p = start + namelen

tilde = streams.get("#~") or streams.get("#-")
strings_off = streams["#Strings"][0]
til = tilde[0]
heapsizes = u8(til + 6)
str_wide  = bool(heapsizes & 1)
guid_wide = bool(heapsizes & 2)
blob_wide = bool(heapsizes & 4)
valid = u64(til + 8)
present = [i for i in range(64) if (valid >> i) & 1]
rp = til + 24
rows = {}
for t in present:
    rows[t] = u32(rp); rp += 4
tables_start = rp

def getstr(idx):
    o = strings_off + idx
    e = o
    while d[e] != 0: e += 1
    return d[o:e].decode("utf-8", "replace")

S = 4 if str_wide else 2
G = 4 if guid_wide else 2
B = 4 if blob_wide else 2
def tidx(t):  # simple index into table t
    return 4 if rows.get(t, 0) >= (1 << 16) else 2

CODED = {
 'TypeDefOrRef':        ([2,1,27], 2),
 'HasConstant':         ([4,8,23], 2),
 'HasCustomAttribute':  ([6,4,1,2,8,9,10,0,14,23,17,20,18,1,27,32,35,38,39,40,42,43], 5),
 'HasFieldMarshal':     ([4,8], 1),
 'HasDeclSecurity':     ([2,6,32], 2),
 'MemberRefParent':     ([2,1,26,6,27], 3),
 'HasSemantics':        ([20,23], 1),
 'MethodDefOrRef':      ([6,10], 1),
 'MemberForwarded':     ([4,6], 1),
 'Implementation':      ([38,35,39], 2),
 'CustomAttributeType': ([-1,-1,6,10,-1], 3),
 'ResolutionScope':     ([0,26,35,1], 2),
 'TypeOrMethodDef':     ([2,6], 1),
}
def cidx(name):
    tabs, bits = CODED[name]
    maxr = 0
    for t in tabs:
        if t >= 0:
            maxr = max(maxr, rows.get(t, 0))
    return 4 if maxr >= (1 << (16 - bits)) else 2

# column schema per table id -> list of column sizes (resolved to byte counts), with markers for str
def col(kind):
    if kind == 'u16': return ('f', 2)
    if kind == 'u32': return ('f', 4)
    if kind == 's':   return ('s', S)
    if kind == 'g':   return ('f', G)
    if kind == 'b':   return ('f', B)
    if isinstance(kind, tuple) and kind[0] == 't': return ('f', tidx(kind[1]))
    if isinstance(kind, tuple) and kind[0] == 'c': return ('f', cidx(kind[1]))
    raise ValueError(kind)

SCHEMA = {
 0:  ['u16','s','g','g','g'],
 1:  [('c','ResolutionScope'),'s','s'],
 2:  ['u32','s','s',('c','TypeDefOrRef'),('t',4),('t',6)],
 4:  ['u16','s','b'],
 6:  ['u32','u16','u16','s','b',('t',8)],
 8:  ['u16','u16','s'],
 9:  [('t',2),('c','TypeDefOrRef')],
 10: [('c','MemberRefParent'),'s','b'],
 11: ['u8','s'] if False else [('c','HasConstant')],  # placeholder fixed below
}
# Full proper schemas (II.22). Define all that can be present.
SCHEMA = {
 0:  ['u16','s','g','g','g'],
 1:  [('c','ResolutionScope'),'s','s'],
 2:  ['u32','s','s',('c','TypeDefOrRef'),('t',4),('t',6)],
 3:  [('t',4)],                       # FieldPtr
 4:  ['u16','s','b'],                  # Field
 5:  [('t',6)],                        # MethodPtr
 6:  ['u32','u16','u16','s','b',('t',8)],   # MethodDef
 7:  [('t',8)],                        # ParamPtr
 8:  ['u16','u16','s'],                # Param
 9:  [('t',2),('c','TypeDefOrRef')],   # InterfaceImpl
 10: [('c','MemberRefParent'),'s','b'],# MemberRef
 11: ['u8b1','c_HasConstant','b'],     # Constant (special: 1-byte type + pad)
 12: [('c','HasCustomAttribute'),('c','CustomAttributeType'),'b'],
 13: [('c','HasFieldMarshal'),'b'],
 14: ['u16',('c','HasDeclSecurity'),'b'],
 15: ['u16','u16',('t',2)],            # ClassLayout
 16: ['u32',('t',4)],                  # FieldLayout
 17: ['b'],                            # StandAloneSig
 18: [('t',2),('t',14)],               # EventMap
 19: [('t',20)],                       # EventPtr
 20: ['u16','s',('c','TypeDefOrRef')], # Event
 21: [('t',2),('t',23)],               # PropertyMap (PropertyList -> Property=23)
 22: [('t',23)],                       # PropertyPtr
 23: ['u16','s','b'],                  # Property
 24: ['u16',('t',6),('c','HasSemantics')], # MethodSemantics
 25: [('t',2),('c','MethodDefOrRef'),('c','MethodDefOrRef')], # MethodImpl
 26: ['s'],                            # ModuleRef
 27: ['b'],                            # TypeSpec
 28: ['u16',('c','MemberForwarded'),'s',('t',26)], # ImplMap
 29: ['u32',('t',4)],                  # FieldRVA
 32: ['u32','u16','u16','u16','b','s','s'], # Assembly
 33: ['u32'],                          # AssemblyProcessor
 34: ['u32','u32','u32'],              # AssemblyOS
 35: ['u16','u16','u16','u16','u32','b','s','s','b'], # AssemblyRef
 36: ['u32'],                          # AssemblyRefProcessor
 37: ['u32','u32','u32',('t',35)],     # AssemblyRefOS
 38: ['u32','s','b'],                  # File
 39: ['u32','u32','s',('c','Implementation')], # ExportedType
 40: ['u32','u32','s',('c','Implementation')], # ManifestResource
 41: [('t',2),('t',2)],                # NestedClass (NestedClass, EnclosingClass)
 42: ['u16','u16',('c','TypeOrMethodDef'),'s'], # GenericParam
 43: [('c','MethodDefOrRef'),'b'],     # MethodSpec
 44: [('t',42),('c','TypeDefOrRef')],  # GenericParamConstraint
}

def colsize(kind):
    if kind == 'u8b1': return 1
    if kind == 'u16': return 2
    if kind == 'u32': return 4
    if kind == 's':   return S
    if kind == 'g':   return G
    if kind == 'b':   return B
    if kind == 'c_HasConstant': return cidx('HasConstant')
    if isinstance(kind, tuple) and kind[0]=='t': return tidx(kind[1])
    if isinstance(kind, tuple) and kind[0]=='c': return cidx(kind[1])
    raise ValueError(kind)

def rowsize(t):
    return sum(colsize(k) for k in SCHEMA[t])

# compute table offsets
offsets = {}
cur = tables_start
for t in present:
    offsets[t] = cur
    if t not in SCHEMA:
        raise ValueError("no schema for table 0x%x (rows %d)" % (t, rows[t]))
    cur += rows[t] * rowsize(t)

def read_col(base, t, colidx):
    o = offsets[t] + base*rowsize(t)
    for i in range(colidx):
        o += colsize(SCHEMA[t][i])
    sz = colsize(SCHEMA[t][colidx])
    if sz == 2: return u16(o)
    if sz == 4: return u32(o)
    if sz == 1: return u8(o)
    return None

# TypeDef table id 2: cols [Flags, Name, Namespace, Extends, FieldList, MethodList]
ntypes = rows.get(2, 0)
nmeth  = rows.get(6, 0)
print("tables present:", [hex(t) for t in present])
print("TypeDef rows:", ntypes, " MethodDef rows:", nmeth)

def type_methods_range(i):
    start = read_col(i, 2, 5)  # MethodList (1-based index into MethodDef)
    if i+1 < ntypes:
        end = read_col(i+1, 2, 5)
    else:
        end = nmeth + 1
    return start, end

found_types = []
for i in range(ntypes):
    name = getstr(read_col(i, 2, 1))
    ns   = getstr(read_col(i, 2, 2))
    if ns == "GodotPlugins.Game" or (name == "Main" and "GodotPlugins" in ns) or "GodotPlugins" in ns:
        found_types.append((i, ns, name))

print("\n== Types in namespace containing GodotPlugins ==")
for i, ns, name in found_types:
    print(f"  [{i}] {ns}.{name}")
    s, e = type_methods_range(i)
    for m in range(s, e):
        mi = m - 1
        if 0 <= mi < nmeth:
            mn = getstr(read_col(mi, 6, 3))
            flags = read_col(mi, 6, 2)
            print(f"    - method: {mn}  (flags=0x{flags:04x})")

# direct search: any MethodDef named InitializeFromGameProject / godotsharp_game_main_init
print("\n== Direct method-name search ==")
for target in ["InitializeFromGameProject", "godotsharp_game_main_init"]:
    hits = []
    for mi in range(nmeth):
        if getstr(read_col(mi, 6, 3)) == target:
            hits.append(mi)
    print(f"  {target}: {len(hits)} hit(s) -> rows {hits[:5]}")
