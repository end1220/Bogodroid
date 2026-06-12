#!/usr/bin/env python3
import struct, sys
def info(path):
    d = open(path, "rb").read()
    u16 = lambda o: struct.unpack_from("<H", d, o)[0]
    u32 = lambda o: struct.unpack_from("<I", d, o)[0]
    e = u32(0x3C)
    assert d[e:e+4] == b"PE\x00\x00"
    coff = e + 4
    machine = u16(coff)
    nsec = u16(coff+2)
    optsz = u16(coff+16)
    opt = coff + 20
    magic = u16(opt)          # 0x10B PE32, 0x20B PE32+
    if magic == 0x20B:
        nrva = u32(opt+108); dd = opt+112
    else:
        nrva = u32(opt+92);  dd = opt+96
    cli_rva = u32(dd + 14*8); cli_sz = u32(dd+14*8+4)
    # map rva->off
    secs=[]
    so = opt + optsz
    for i in range(nsec):
        b = so+i*40
        secs.append((u32(b+12), u32(b+8), u32(b+20), u32(b+16)))
    def r2o(rva):
        for va,vs,praw,sraw in secs:
            if va <= rva < va+max(vs,sraw): return praw+(rva-va)
        return None
    machines = {0x14c:"I386", 0x8664:"AMD64", 0xaa64:"ARM64", 0x1c4:"ARMNT", 0x200:"IA64"}
    res = {"file":path.split('/')[-1], "machine":machines.get(machine,hex(machine)),
           "pe":"PE32+" if magic==0x20B else "PE32"}
    if cli_rva:
        c = r2o(cli_rva)
        cb = u32(c)
        flags = u32(c+16)
        # CLI header: cb(4) rtMajor(2) rtMinor(2) MetaRVA(4) MetaSize(4) Flags(4) EntryPoint(4)
        # then DataDirectories: Resources(8), StrongName(8), CodeManagerTable(8),
        # VTableFixups(8), ExportAddressTableJumps(8), ManagedNativeHeader(8)
        mnh_rva = u32(c+16+4+4+8+8+8+8+8)      # ManagedNativeHeader RVA
        mnh_sz  = u32(c+16+4+4+8+8+8+8+8+4)
        fl=[]
        if flags & 0x1: fl.append("ILONLY")
        if flags & 0x2: fl.append("32BITREQUIRED")
        if flags & 0x8: fl.append("STRONGNAMESIGNED")
        if flags & 0x10: fl.append("NATIVE_ENTRYPOINT")
        if flags & 0x20000: fl.append("32BITPREFERRED")
        res["cli_flags"] = "|".join(fl) or hex(flags)
        res["flags_raw"] = hex(flags)
        res["R2R(ManagedNativeHeader)"] = f"rva=0x{mnh_rva:x} size={mnh_sz}" + ("  <-- ReadyToRun!" if mnh_rva else "  (none, pure IL)")
    return res

for p in sys.argv[1:]:
    r = info(p)
    print(f"== {r['file']} ==")
    for k,v in r.items():
        if k!='file': print(f"   {k}: {v}")
    print()
