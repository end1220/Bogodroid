#!/usr/bin/env python3
"""
跨 RID 改写自包含 .NET 应用的 <app>.deps.json(例:win-x64 → linux-arm64)。

.NET 的 IL 程序集跨架构通用,只有 deps.json 的 RID 元数据 + 原生库文件名是平台相关的。
本脚本:
  - 把 RID 替换进 runtimeTarget.name、targets 外层 key、runtimepack key、所有 libraries key
  - 用数据文件夹里**实际存在的 *.so** 重建 runtimepack 的 'native' 映射
    (coreclr.dll/clrjit.dll → libcoreclr.so/libclrjit.so …),并排除走宿主链的 libhostfxr/libhostpolicy
不改写的话,godot/hostfxr 会报 "Could not resolve CoreCLR path"。

【给谁用】移植 C# .NET Godot(或任何自包含 .NET 应用)到另一架构时,组好新架构数据文件夹后跑一次。
前置:数据文件夹里**已放好**新架构的运行时 .so + 游戏 IL 程序集 + runtimeconfig.json。

用法:
    python3 rewrite_deps_json.py <数据文件夹> [--old win-x64] [--new linux-arm64]
会先备份 <app>.deps.json.<old>.bak。
"""
import sys, os, json, glob, shutil, argparse


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data_folder", help="含 *.deps.json / 新架构 .so / IL 程序集的文件夹")
    ap.add_argument("--old", default="win-x64")
    ap.add_argument("--new", default="linux-arm64")
    a = ap.parse_args()
    OLD, NEW, ddir = a.old, a.new, a.data_folder

    deps_files = glob.glob(os.path.join(ddir, "*.deps.json"))
    if len(deps_files) != 1:
        print("ABORT: 期望恰好 1 个 *.deps.json,实际:", deps_files); sys.exit(1)
    deps = deps_files[0]
    d = json.load(open(deps))

    # 新架构实际存在的原生库;hostfxr/hostpolicy 走宿主链,不进 native 映射
    host_excl = {"libhostfxr.so", "libhostpolicy.so"}
    have_so = sorted(f for f in os.listdir(ddir) if f.endswith(".so") and f not in host_excl)

    # 1) runtimeTarget.name
    d["runtimeTarget"]["name"] = d["runtimeTarget"]["name"].replace(OLD, NEW)

    # 2) targets:外层 key 改名;内部 runtimepack key 改名 + 用实际 .so 重建 native
    targets = d["targets"]
    for k in list(targets):
        if OLD in k:
            targets[k.replace(OLD, NEW)] = targets.pop(k)
    for tval in targets.values():
        for pk in list(tval):
            entry = tval[pk]
            if "native" in entry:
                entry["native"] = {so: {"fileVersion": "0.0.0.0"} for so in have_so}
            npk = pk.replace(OLD, NEW)
            if npk != pk:
                tval[npk] = tval.pop(pk)

    # 3) libraries:key 改名
    libs = d["libraries"]
    for k in list(libs):
        if OLD in k:
            libs[k.replace(OLD, NEW)] = libs.pop(k)

    shutil.copy(deps, deps + "." + OLD + ".bak")
    json.dump(d, open(deps, "w"), indent=2)
    print("已改写 %s:%s → %s | native 库 %d 个" % (os.path.basename(deps), OLD, NEW, len(have_so)))
    print("注意:若某个游戏库自带 win 专属 native 依赖(非 runtimepack),需手动核对。")


if __name__ == "__main__":
    main()
