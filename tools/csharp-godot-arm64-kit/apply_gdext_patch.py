#!/usr/bin/env python3
"""
就地给一个 Godot .pck 打补丁:把某个 res://.../*.gdextension 整个替换成预生成的新内容
(本套件用于给 SlayTheSpire2.pck 的 fmod.gdextension 补上 linux.arm64 条目)。

只改 gdextension 那一小段(本例 3767 字节)+ 目录里它的 16 字节 md5;1.85 GB 的包体和所有
文件偏移都不动。幂等(已打过会跳过)、带原内容 md5 校验(pck 不对会拒)。

【给谁用】要让 arm64 的 Godot 加载某原生 GDExtension,但 pck 里的 .gdextension 缺当前 arch 条目时。
补丁数据(确切偏移/md5/新内容)是 **game-specific** 的,放在 patch_dir(默认 ./examples)里:
  - patch_meta.json     {gdext_ofs, gdext_size, md5_pos, orig_md5, new_md5}
  - fmod.gdextension.patched   填充到 gdext_size 的新内容

换一个游戏/pck:用本套件的 peflags/解析思路重新定位偏移、生成新内容,产出一套新的 examples 即可。

用法:
    python3 apply_gdext_patch.py <pck> [patch_dir]      # patch_dir 默认 ./examples
"""
import sys, os, json, hashlib

if not (2 <= len(sys.argv) <= 3):
    print(__doc__); sys.exit(2)
pck = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
patch_dir = sys.argv[2] if len(sys.argv) == 3 else os.path.join(here, "examples")

meta = json.load(open(os.path.join(patch_dir, "patch_meta.json")))
blob = open(os.path.join(patch_dir, "fmod.gdextension.patched"), "rb").read()
assert len(blob) == meta["gdext_size"], "blob size mismatch"

with open(pck, "r+b") as f:
    f.seek(meta["gdext_ofs"])
    cur = f.read(meta["gdext_size"])
    cur_md5 = hashlib.md5(cur).hexdigest()
    if cur_md5 == meta["new_md5"]:
        print("already patched, nothing to do"); sys.exit(0)
    if cur_md5 != meta["orig_md5"]:
        print("ABORT: bytes at gdext offset have md5 %s, expected original %s.\n"
              "Wrong pck or different build — not touching it." % (cur_md5, meta["orig_md5"]))
        sys.exit(1)
    f.seek(meta["gdext_ofs"]); f.write(blob)
    f.seek(meta["md5_pos"]);   f.write(bytes.fromhex(meta["new_md5"]))
    print("patched OK: fmod.gdextension now lists linux.release/debug/editor.arm64")
