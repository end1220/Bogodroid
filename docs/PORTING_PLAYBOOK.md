# Unity IL2CPP → Linux ARM 掌机移植 Playbook

面向 1GB 级 UMA 掌机（如 Anbernic BuildRoot）。新端口**先读本文**与 [`CASE_STUDIES.md`](CASE_STUDIES.md)；大文件推送见根目录 [`AGENTS.md`](../AGENTS.md)。

## 0. 何时不要开坑（止损判据）

移植前用解包 + `tools/unity_astc/inventory_bundle.py` 粗估：

| 红灯 | 说明 |
|------|------|
| 标题/菜单阶段 Addressables 热包贴图+音频已近物理内存 | 进关再加载必顶穿 |
| 单个 bundle 同时塞「全角色/全音效/全 UI」 | 无法部分卸载；缩纹理只能延缓 |
| 运行时压 ASTC/ETC2 无效 | `textureMaxDim` **拦不住**压缩上传，必须离线 retier |

**负面案例：Skul（已止损）** — 摘要见 [`CASE_STUDIES.md`](CASE_STUDIES.md)。标题 RSS≈785MB 后进关无意义。

## 1. 标准流水线

```text
APK/解包 → 本地 staging
    → inventory / astc_retier / shrink / audio_stream_patch
    → Docker：Release + 关日志编 unityloader + plugin
    → Dropbeak push (--force --chunk --chunk-size 16m --verify)
    → 掌机 Ports 手启（勿随意远程 .sh）
    → 看 log：退出码、prefs flush；诊断构建再开 [BD-MEM]
```

### 1.1 发布构建（小体积、少日志）

日常上机用 **Release、关 BD 日志、strip**（约 5MB 级，而非 Debug ~90MB）。

日志为**编译期**开关（见 `platform/common/logging.h` / `AGENTS.md`）：

| 开关 | 上机 | 排障 |
|------|------|------|
| `BD_ENABLE_LOG` | OFF | ON（主开关，含 `[BD-MEM]`） |
| `BD_ENABLE_TRACE` | OFF | 按需 ON |
| `BD_ENABLE_VERBOSE` | OFF | 仅深挖时 ON（行数极多） |
| `CMAKE_BUILD_TYPE` | Release + strip | Debug 或 Release+LOG |

```powershell
docker run --rm --platform linux/amd64 `
  -v "D:\Locke\gitee\Bogodroid:/work" -w /work/build-aarch64 `
  bogo-builder:unity2017-armv7 bash -c @"
cmake . -DCMAKE_BUILD_TYPE=Release \
  -DBD_ENABLE_LOG=OFF -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF
cmake --build . -j2 --target unityloader plugin_<name>
aarch64-linux-gnu-strip --strip-unneeded unityloader unityloader.d/*.so
"@
```

排障示例：同一目录改 `-DBD_ENABLE_LOG=ON`，可选 `-DBD_ENABLE_TRACE=ON`，重编推送；toml `[debug] mem_log_interval_ms` 仅在 LOG 打开时有输出。

## 2. 资产工具矩阵

| 工具 | 用途 |
|------|------|
| `inventory_bundle.py` | 只读：贴图 VRAM / 音频 / 类型占比 |
| `astc_retier.py` / `retier_all.sh` | 强制 ASTC；`--packer original` |
| `shrink_bundle.py --cap N` | 降长边；磁盘可能变大，看 RSS |
| `audio_stream_patch.py` | LoadType；内嵌包加 `--allow-embedded --mode hybrid` |

优先看**运行时 RSS**，不要只看磁盘体积。

## 3. 核心能力（勿再写进标题插件）

| 能力 | 配置 / API |
|------|------------|
| 进程内存 | `[debug] mem_log_interval_ms`；需 `BD_ENABLE_LOG`；标签 `[BD-MEM]` |
| PAD Java stub | `[play_asset_delivery] enabled/default_pack/pack_version/pack_path_template` |
| 每帧 present 钩子 | Plugin ABI v3：`register_present_callback` |
| CPU present / swap pause | `BD_EGL_CPU_PRESENT`、`BD_EGL_SWAP_PAUSE`（插件可 setenv） |

标题专属逻辑放 `unityloader.d/<game>.so`。

## 4. 插件约定

1. `abi_version == BOGODROID_PLUGIN_ABI_VERSION`（当前 **3**）。
2. 每帧工作用 `register_present_callback`，不要让核心 `dlsym` 游戏名符号。
3. PAD 路径补丁 / IL2CPP hook 留在插件。

## 4.1 单机掌机：优先跳过在线 SDK（重要经验）

掌机 Ports **默认无 Google Play / 推送 / 广告归因**。Boot 里若串了 Adjust / Firebase / AppLovin(MAX) / Play Games：

| 做法 | 何时用 |
|------|--------|
| **IL2CPP 早跳过**托管入口（`InitFirebase`→继续 `StartGame`，空掉 `*Manager.Initialization`） | **优先**。目标是让游戏进 `BundleManager` / 主场景，而不是把 SDK 跑通 |
| JNI `sdk-skip` / FakeJni 空 stub | 反射 `AndroidJavaObject` 调用时防 NRE；**不能**代替跳过 Boot 门闩 |
| 大面积 `dlsym` 假实现 Firebase C++ | **慎用**。假指针/`strdup` 易 `free()`/`double free` ABRT；最多只 nop `SWIGRegister*` |

反例（PC01）：花大量时间修 Adjust/Firebase JNI 与 native stub，引擎已能刷帧却长期黑屏；一旦 `InitFirebase` bypass→`StartGame`，立刻进 splash。  
细则与当前卡点见 [`PC01_HANDOFF.md`](PC01_HANDOFF.md)。

## 5. 选游戏与引擎初判

| 信号 | 含义 |
|------|------|
| `libil2cpp.so` + `global-metadata.dat` | Unity IL2CPP → 本仓库主线 |
| `libyoyo.so` | GameMaker → **不是** unityloader 路径（另起运行时） |
| 大 Addressables / Play Asset Delivery 包 | 先做 inventory；标题峰值近 1GB 则止损 |

## 6. 复测清单

1. `dropbeak-cli ping`；大文件 `ls -la` / `--verify` 一致。  
2. 正常退出：`exited (0)` + prefs flush。  
3. 诊断构建下看 `[BD-MEM]`；标题阶段 `sys_avail` &lt;100MB → 止损或换游戏。
