# csharp-godot-arm64-kit · C# Godot 游戏 → arm64 掌机 移植套件

把 **C#/.NET Godot 游戏**(Windows x64 导出)搬到 **aarch64 Linux 掌机**(如 MiniLoong / RK3566)时用的
诊断 + 补丁脚本。配套**方法文档**:[`docs/miniloong/SlayTheSpire2_CSharp-Godot-arm64移植.md`](../../docs/miniloong/SlayTheSpire2_CSharp-Godot-arm64移植.md)
(按阶段讲清每步:目标→问题→定位→解决→注意,含『注意事项总表』)。

> 首例对象是 Slay the Spire 2;脚本本身是通用的,`examples/` 里是 StS2 专属的补丁数据(换游戏要重新生成)。

## 这是给谁用的

给**做 C# Godot 移植的人 / 后续接手的 AI**。不含游戏文件、不含编译产物、不含闭源库——只有"工具"。
按下面流水线顺序用。

## 流水线(用脚本的顺序)

| 步骤 | 工具 | 给谁用 / 何时用 | 用法 |
|---|---|---|---|
| ① 组新架构 .NET 运行时后 | **`rewrite_deps_json.py`** | 把自包含 .NET 应用从 win-x64 搬到 linux-arm64:跨 RID 改写 `deps.json`,否则报 `Could not resolve CoreCLR path` | `python3 rewrite_deps_json.py <DATA夹>` |
| ② 排查 .NET 加载失败 | **`peflags.py`** | 看程序集 PE machine / PE32+ / CLI flags / 是否 R2R——判断是不是 x64 标记 | `python3 peflags.py a.dll b.dll …` |
| ② 排查(确认类型在不在) | **`clrmeta.py`** | 解析 ECMA-335 元数据,确认某类型/方法确实在程序集内(排除"找不到"误判) | `python3 clrmeta.py app.dll` |
| ② 抓真实异常 | **`probe/`** | godot 报 GodotPlugins null 又抓不到真因时:复刻 `load_assembly_and_get_function_pointer`、catch 打印。会吐出 `architecture is not compatible` 这种真话 | 见 `probe/README` 下方 |
| ③ 修 x64 标记程序集 | **`patch_pe_machine.py`** | `<PlatformTarget>x64>` 的纯 IL 程序集被 arm64 coreclr 拒:PE machine `AMD64→ARM64`(每个 2 字节) | `python3 patch_pe_machine.py <DATA夹> [--dry]` |
| ④ 补 pck 内文件(同长度原地) | **`apply_pck_blob_patch.py`** + `examples/` | 通用 pck 内文件同长度替换 + 自动更新目录 md5。当前两组 examples:**fmod gdextension** 加 linux.arm64 条目、**project.binary** stub 掉 SentryInit autoload 消除 parse error 墙。 | `python3 apply_pck_blob_patch.py <pck> <meta.json> <blob>`(无参=fmod 默认) |
| ④' 旧名兼容 | `apply_gdext_patch.py` | 等价旧版,仅用 fmod 默认补丁。新代码用 ④ 那个 | — |
| ⑤ 编原生 arm64 扩展 | **`ci/*.yml`** | fmod / spine 等无 linux-arm64 版的 GDExtension:Fork 上游 + 这套 workflow 用 GitHub arm64 runner 自编 | 见下「CI workflow」 |

## probe 用法

`probe/`(Program.cs + probe.csproj)是个自包含 net9.0 控制台,把游戏主程序集按路径加载、取
`GodotPlugins.Game.Main.InitializeFromGameProject` 的函数指针,逐步 catch 打印。
```
# Mac 上(有 dotnet SDK):
dotnet publish -c Release -r linux-arm64 --self-contained -o out probe/probe.csproj
# 把 out/ 里的 4 个小文件(probe / probe.dll / *.runtimeconfig.json / *.deps.json)放进设备上
# 现成的自包含 .NET 运行时夹(= 游戏的 DATA 夹),chmod +x probe && ./probe
```
> csproj 里 `<RuntimeFrameworkVersion>` 要锁成与设备一致(本例 9.0.7),才能复用 DATA 夹的运行时。

## CI workflow（`ci/`）

- `fmod_build-linux-arm64.yml` → 配 `utopia-rise/fmod-gdextension` 的 Fork(分支 `master`)。
  **需要** 仓库 secret `FMODUSER`/`FMODPASS`(免费 FMOD 账号)。自包含,不用上游 composite action。
  产 `libGodotFmod.linux.*.arm64.so`(GLIBC ≤ 2.17)+ FMOD 运行时 `libfmod.so.14.6` / `libfmodstudio.so.14.6`。
- `spine_build-linux-arm64.yml` → 配 `EsotericSoftware/spine-runtimes` 的 Fork(分支须匹配骨架版本,
  本例 `4.2`)。**不需要 secret**。产 `libspine_godot.linux.*.arm64.so`(GLIBC ≤ 2.29)。
- 两者都用原生 `ubuntu-24.04-arm` runner **+ `container: ubuntu:20.04`**(关键:容器 glibc 2.31,
  覆盖典型 PortMaster 掌机 2.31-2.33);末尾都有 `objdump -T *.so | grep GLIBC_` 自检步骤。
  godot-cpp 锁 `4.5`(=4.5.0)≤ 游戏 Godot 版本。

> ⚠️ GitHub 坑(详见方法文档总表):
> ① 推 `.github/workflows/*` 需 token 的 `workflow` scope(默认没有)→ 用**网页 UI**加/改;
> ② `workflow_dispatch` 只在**默认分支**显示 → 把 fork 默认分支切到目标分支;
> ③ job `name:` 不能用 `${{ env.X }}`,会 0 秒解析失败;
> ④ runner glibc 比掌机新太多 → **必须用 `container: ubuntu:20.04`**,否则 .so 在掌机加载报 `GLIBC_2.XX not found`。

## examples/(StS2 专属)

- `fmod.gdextension.patched`、`patch_meta.json` —— 给 `apply_gdext_patch.py` 的输入,**仅对 StS2 那个 pck 有效**
  (偏移 / md5 都是那个具体 build 的)。换游戏需重新定位偏移、重新生成。

## 不在这里的东西(故意排除)

- FMOD 闭源运行时 `libfmod*.so` —— 不能进公库;从 fmod fork 的 CI 产物取。
- 游戏自身的 .NET 程序集 / pck —— 版权;自备。
- 编好的 `.so` / data 文件夹 —— 可由 fork(CI)+ 上面的脚本重建。
