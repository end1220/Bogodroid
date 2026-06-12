#!/usr/bin/env python3
"""
就地给一个 Godot .pck 打补丁:把 pck 内某个文件**整个**替换成同长度的新内容
(不重写 1.85 GB,只改那一小段 + 更新 pck 目录里该文件的 16 字节 md5)。

通用版,支持任意 pck 内文件(本套件 examples/ 里目前有两组 StS2 专属补丁):
  - **fmod.gdextension**     给 res://addons/fmod/fmod.gdextension 加 linux.arm64 条目
  - **project.binary**       把 autoload/SentryInit 路径改成同长度的不存在 stub
                              (godot 跳过该 autoload → 整面 sentry parse error 墙消失)

补丁数据**一组两个文件**放在 patch_dir(默认本目录 examples/):
  - <meta>.json        {blob_ofs, blob_size, md5_pos, orig_md5, new_md5, description}
  - <blob_file>        填充到 blob_size 的新内容

幂等(已打过会跳过)、带原内容 md5 校验(pck 不对会拒)。

【给谁用】移植时,要把 pck 内某个**与 arch 相关的、纯文本/配置类**文件替换成同长度版本时。
换 pck/换游戏:重新解析 pck 找到目标文件的绝对偏移 + 目录项 md5 位置 + 原 md5,生成一对新文件。

用法:
    python3 apply_pck_blob_patch.py <pck> <meta.json> <blob_file>
    # 不带参 = 打默认的 fmod patch(向后兼容 apply_gdext_patch.py 的用法)
    python3 apply_pck_blob_patch.py <pck>

示例:
    # 1) fmod 加 linux.arm64 条目
    python3 apply_pck_blob_patch.py game.pck examples/patch_meta.json examples/fmod.gdextension.patched
    # 2) sentry 自动加载 stub 掉
    python3 apply_pck_blob_patch.py game.pck examples/project_binary_meta.json examples/project.binary.sentry_stub
"""
import sys, os, json, hashlib

here = os.path.dirname(os.path.abspath(__file__))
if len(sys.argv) == 2:                      # 兼容旧用法:仅 pck → fmod patch
    pck = sys.argv[1]
    meta_path = os.path.join(here, "examples", "patch_meta.json")
    blob_path = os.path.join(here, "examples", "fmod.gdextension.patched")
elif len(sys.argv) == 4:                    # 通用:pck + meta + blob
    pck, meta_path, blob_path = sys.argv[1], sys.argv[2], sys.argv[3]
else:
    print(__doc__); sys.exit(2)

meta = json.load(open(meta_path))
blob = open(blob_path, "rb").read()
# 兼容旧元数据(gdext_ofs/gdext_size)和新元数据(blob_ofs/blob_size)字段
ofs = meta.get("blob_ofs",  meta.get("gdext_ofs"))
sz  = meta.get("blob_size", meta.get("gdext_size"))
assert ofs is not None and sz is not None, "meta 缺 blob_ofs/blob_size 字段"
assert len(blob) == sz, f"blob 大小 {len(blob)} 不匹配元数据 {sz}"

with open(pck, "r+b") as f:
    f.seek(ofs)
    cur = f.read(sz)
    cur_md5 = hashlib.md5(cur).hexdigest()
    if cur_md5 == meta["new_md5"]:
        print(f"already patched at @{ofs}, nothing to do"); sys.exit(0)
    if cur_md5 != meta["orig_md5"]:
        print(f"ABORT: bytes at @{ofs} have md5 {cur_md5}, expected original {meta['orig_md5']}.\n"
              f"Wrong pck or different build — not touching it."); sys.exit(1)
    f.seek(ofs);              f.write(blob)
    f.seek(meta["md5_pos"]);  f.write(bytes.fromhex(meta["new_md5"]))
    print(f"patched OK @{ofs} ({sz}B). {meta.get('description','')}")
