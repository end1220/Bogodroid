# 标题专属代码清单（Title-Specific Code Inventory）

本文件盘点 `oddmar` 分支树里**所有标题专属（title-specific）代码与非代码资产**，用于判断哪些该留在核心、哪些该抽进插件。

## 扫描口径

| 项 | 值 |
|---|---|
| 分支 / HEAD | `oddmar` @ `6453067` |
| 文件类型 | `*.cpp *.h *.c *.hpp *.toml *.sh *.py *.md *.json` |
| 排除 | `_trimui_logs/`、`build*/`、`libjnivm/{tests,examples}`、`tomlplusplus/` |
| 判定 | 严格区分**代码命中**与**仅注释命中**；仅注释引用不算"专属代码" |

标题标记词：`oddmar`、`mobge`、`senri`、`hollow knight`、`skul`、`samurai`、`madfinger`、`maximus`、`terraria`、`fivehearts`、`heishenhua`、`limbo`、`playdead`、`hexagon`、`teapot`。

> ⚠️ 本清单**只覆盖当前分支树**，不构成 154 个提交的逐条归属核定。另外 `main` / `unity6` 与本分支**无共同祖先**（`git merge-base` 为空），因此不能用 `main` 做差异基线。

## 2026-10-08 复核摘要

本次按当前 HEAD 重新核对后，原清单的三大核心结论已更新为：

1. **Oddmar 专属代码已从 A1 核心移出**：输入闸门与 Oddmar IL2CPP RVA hooks 已进入 `oddmar_input` 插件；视频路径翻译已进入 `oddmar_video` 插件；`com.mobge.assetlocator.*` 已进入 `oddmar_assetlocator` 插件。
2. **通用视频链路仍可留核心**：`javastubs/bd_video.cpp` 与 GLES 外部纹理路径没有标题硬编码；问题是 Oddmar 私有资源协议把本地路径喂给它。
3. **插件 ABI 已扩展到 v8**：在 v6 的模块、输入、present 和 JNI class 注册之上，v7 增加宿主拥有的对象 userdata、对象创建、byte array 与 string array C ABI；v8 增加通用触摸注入与 present viewport 设置。A3 已用 v7 能力完整插件化，BrickGamePro 使用 v8 能力隔离标题专属输入/视口逻辑。

当前状态：**A1、A2、A3 均已完成并通过容器回放**。A3 的早期实验曾直接跨 DSO 共享 jnivm C++ descriptor/VM 状态，已确认会触发 `std::system_error`/`pthread_mutex_lock` 崩溃；现行实现改为宿主 C ABI，已避开该边界。

### FFmpeg 可选运行时加载真机回归（2026-10-09）

本轮把 FFmpeg 从 `unityloader` 的链接期依赖改为 Android media bridge 首次使用时 `dlopen`/`dlsym` 加载，并手工完成三款 TrimUI Smart Pro 真机回归：

| 游戏 | 部署内容 | 结果 |
|---|---|---|
| Oddmar | `unityloader` + `oddmar_input.so` + `oddmar_video.so` + `oddmar_assetlocator.so`，启动脚本保留 FFmpeg 4.2 `LD_LIBRARY_PATH` | 通过：片头/菜单视频路径正常，三个 Oddmar 插件均加载，未见 FFmpeg 链接期缺库崩溃 |
| Maximus2 | 仅 `unityloader`，不随包携带 FFmpeg | 通过：无视频端口可启动运行，证明未使用 Android media 时不需要 `libav*.so` |
| Samurai2 | `unityloader` + `samurai2_offline.so`，不随包携带 FFmpeg | 通过：插件加载和离线补丁正常，未因缺少 FFmpeg 运行库影响启动 |

结论：FFmpeg 现在是**视频路径的可选运行时依赖**。Oddmar 这类使用视频的游戏仍需在启动脚本里提供匹配的 `libav*.so.58` 搜索路径；Maximus2、Samurai2 这类不走视频链路的端口不再需要复制 FFmpeg 库。

### A3 容器验收记录（2026-10-08）

- 构建目录：`GlES_Dev:/workspace/Bogodroid/build-a3-plugin-log`。
- 构建开关：`Release/日志主开关 ON` 的同等日志配置、`BD_ENABLE_MOBGE_ASSETLOCATOR=OFF`、`JNIVM_ENABLE_RETURN_NON_ZERO=OFF`、`JNIVM_ENABLE_DEBUG=ON`。
- 回放：`/game/Oddmar/seq-a3-plugin-log7`，约 30 秒，产生 3 个 loader GL 帧 dump。
- 插件实际执行：`ListAssets`、`GetReaderWrapper('Bundles/bundle1' ... 'bundle8')`；插件对象 userdata、String[]、Reader byte array 路径均已走通。
- 结果：无 `BD-SEGV`，无 `Unable to read header from archive file`。
- 真机状态：本轮未部署。TrimUI 已下线，`172.16.6.214:8080` 也未进行推送或启动操作。下一次上机前必须重新确认联网、核对 loader 与所有 `unityloader.d/*.so` 的 SHA-256，再按 `cmd_to_run.sh` / `trimui_run.sh` 启动。

## 三层架构与判定原则

| 层 | 位置 | 载入时机 | 判据 |
|---|---|---|---|
| 核心 | `thunks/`、`platform/common/`、`javastubs/`、`libjnivm/`、`loader/` | 编译进 `unityloader` | 由运行时探测或 env 开关驱动，无标题硬编码 |
| 插件 | `unityloader.d/*.so` | 运行时 `dlopen`，两阶段 | 含标题硬编码 / 私有包名 / guest 偏移 |
| 编译开关 | `CMakeLists.txt` option | 构建期 | 可选特性组（如 `BD_ENABLE_GPLAY`） |

**结论先行**：通用视频链路（`javastubs/bd_video.cpp` + `thunks/khronos/gles2.cpp`）**一行标题代码都没有**，由运行时探测（`bd_video::has_sink()`、外部纹理 bind）驱动，属核心。标题专属的是**给它喂本地路径的翻译层**。

## A. 核心内标题专属代码（应抽到插件）

### A1. Oddmar 输入闸门（体量最大）

| 位置 | 内容 | 约行数 |
|---|---|---|
| `projects/unityloader/plugins/oddmar_input/oddmar_input_state.h` | `OddmarInputState` 类，**整文件标题专属** | 97 |
| `projects/unityloader/plugins/oddmar_input/oddmar_input.cpp` | gate、7 个 hook、2 个 UI 模板、RVA 安装与配置 | ~360 |
| `platform/common/input_backend.cpp` | 通用 SDL observer 广播；不再包含 Oddmar gate | — |
| `projects/unityloader/main.cpp` | 通用帧边界广播；不再安装 Oddmar RVA hooks | — |

硬编码的 guest 偏移（全部指向 Oddmar 的 `libil2cpp.so`）：

| 插件位置 | 符号 | RVA |
|---|---|---|
| `oddmar_input.cpp` | `pause_toggle` | `0x93047C` |
| `oddmar_input.cpp` | `try_quit` | `0x8E53AC` |
| `oddmar_input.cpp` | `ui_rvas[]` | `0x948BF8` `0x948E04` `0x97D5F0` `0xFD4C48` `0xFD4C68` |
| `oddmar_input.cpp` | `jump` | `0x9483A8` |
| `oddmar_input.cpp` | `raw_button` | `0x17EB88C` |
| `oddmar_input.cpp` | `attack` | `0x9485CC` |
| `oddmar_input.cpp` | `attack2` | `0x9487E8` |
| `oddmar_input.cpp` | `walk` | `0x94DD00` |

配置键：`[input] oddmar_menu_gate`、`[input] oddmar_sdl_buttons`。
包名门控：`packageName == "com.mobge.Oddmar"`。

核心 → 插件的通用回调点：`input_backend.cpp` 广播 button/axis，`main.cpp` 广播 frame begin；插件通过 `register_input_observer()` 接收。

### A2. 视频 URL 翻译（Oddmar `libunity.so+0x4c124c`）

| 位置 | 内容 |
|---|---|
| `main.cpp:37-97` | ABI 契约推导长注释（v1→v4 迭代史） |
| `main.cpp:262-429` | `g_video_root` / `g_video_fallback` / `g_video_last_source`、`bd_gamedata_root`、`bd_video_note_asset_source`、`bd_video_relpath_from_url`、`bypass_video_translate` |
| `main.cpp:1650-1656` | 安装（`BD_BYPASS_VIDEO_TRANSLATE` + 硬编码偏移） |
| `main.cpp:321`、`main.cpp:376` | **硬编码标题资源名** |
| `main.cpp:780` | libunity 版本专属探针偏移 |

```cpp
// main.cpp:319-321 —— 名字取得很通用的 BD_BYPASS_VIDEO_TRANSLATE 里
// 硬编码了 Oddmar 的 splash 文件名
g_video_fallback = g_video_root +
    "/assets/Videos/mobge_and_senri_splash_video.mp4";
```

```cpp
// main.cpp:780 —— probe_fs_source_pub 的单例指针槽（仅诊断用）
const uintptr_t gp = base + 0xf0b000 + 0xB88;    // singleton pointer slot
```

### A3. `com.mobge.assetlocator.*`（发行商私有包）

| 位置 | 内容 | 约行数 |
|---|---|---|
| `projects/unityloader/plugins/oddmar_assetlocator/oddmar_assetlocator.cpp` | `AssetLocator` + `AssetReader` 插件实现 | ~230 |
| `projects/unityloader/plugin_api.h` / `main.cpp` | v7 对象 userdata、对象/数组创建 C ABI | — |
| `javastubs/bd_assetlocator.cpp` | 旧核心过渡实现，保留用于 A/B 和回滚 | 384 |
| `javastubs/android.h:1127-1240` | 两个 jnivm 类声明 + `DEFINE_CLASS_NAME("com/mobge/...")` | ~114 |
| `javastubs/android_descriptors.cpp:107-115` | `registerClass<>()` 注册 | 9 |
| `CMakeLists.txt:235-239` | 由 `BD_ENABLE_MOBGE_ASSETLOCATOR` 控制，默认 OFF；Oddmar 兼容构建显式 ON | 5 |

`com.mobge.*` 是发行商私有包，非 Android/Google 标准包。同类问题核心已有先例：`com.google.*` 走编译开关 `BD_ENABLE_GPLAY`（`CMakeLists.txt:297-302`）。

迁移难点不是“方法表注册”本身。v6 的 `register_jni_class()` 已能挂 C 回调，但 A3 还需要真正的 `AssetReader` 实例、byte array、String[] 以及构造语义。v7 由宿主提供这些能力；对象 userdata 和对象保活由宿主负责，插件只保有自己的 reader/locator 状态。

因此 A3 的合理路径有两种：

| 路径 | 做法 | 适用性 |
|---|---|---|
| 过渡 | **仍保留**：`BD_ENABLE_MOBGE_ASSETLOCATOR` 默认 OFF，Oddmar 构建可显式 ON | 回滚与 A/B 对照 |
| 完整插件化 | **已完成**：ABI v7 对象/数组/byte array 创建、插件私有 userdata、构造/实例化回调 | Oddmar 默认使用 `oddmar_assetlocator.so` |

### A4. 隐藏耦合：A2 ↔ A3 必须同进同出

`javastubs/bd_assetlocator.cpp` 通过核心的通用 asset-source observer 通知视频插件：

| 位置 | 角色 |
|---|---|
| `projects/unityloader/main.cpp` | 提供 `bd_plugin_notify_asset_source()`，广播给已注册插件 |
| `javastubs/bd_assetlocator.cpp:38` | 声明通用通知入口 |
| `javastubs/bd_assetlocator.cpp:154` | 调用 `bd_plugin_notify_asset_source(file.string().c_str())` |

即 **A2 与 A3 是同一套 Oddmar 私有协议的上下游**。最终插件化时应作为同一 Oddmar 兼容包设计；分阶段落地时可先让 A3 留在核心或编译开关里，但 A2 插件仍需要一个明确的 asset-source 通知/查询接口，不能继续靠隐式 `extern "C"` 反向调用。

## B. 标题专属非代码资产

| 类别 | 文件 | 备注 |
|---|---|---|
| FiveHearts 全套 | `scripts/fivehearts/`（17 个文件）、`scripts/fivehearts_launch.sh`、`configs/fivehearts.toml`、`docs/FIVEHEARTS.md` | `scripts/fivehearts/bt.sh` 硬编码 `0x3c2434` `0x3c2a78` |
| Oddmar 工具 | `tools/oddmar_input_smoke.sh`、`tools/oddmar_input_state_test.cpp` | 与 A1 配套 |
| Samurai2 工具 | `tools/localize_samurai2_assets.py` | 插件已抽出，工具仍散落 |
| Hollow Knight 测试 | `tools/tests/test_hk_launcher_blackbars.py`、`tools/tests/test_hk_viewport_diagnostics.py` | 插件已抽出 |
| Oddmar 文档 | `docs/ODDMAR.md`、`docs/ODDMAR-ASSET-SLIMMING.md` | case study |
| 过程记录 | `.workbuddy/memory/2026-09-21.md`、`2026-09-22.md`、`2026-09-23.md`、`MEMORY.md` | 含 Oddmar 案例 |
| 设备配置 | `unity.toml.device` | `packageName="com.mobge.Oddmar"`、`-force-gles31` |
| 其他 loader 配置 | `configs/limbo.toml`、`configs/hexagon.toml`、`configs/teapot.toml` | 对应 D |

`configs/*.toml` 逐标题配置属正常设计，无需改动。

整理建议：这类资产不必强行从仓库删除，但应避免混在通用工具的第一层目录里。建议后续按标题聚合为：

```text
tools/titles/oddmar/
tools/titles/samurai2/
tools/titles/hollow_knight/
scripts/titles/fivehearts/
```

保留 `configs/*.toml` 与 `docs/*.md` 的现状即可；它们是移植档案和可复现实验记录，不是核心污染。

## C. 已抽出的插件（合规）

`projects/unityloader/plugins/`：

| 插件 | 标题 |
|---|---|
| `hollow_knight_viewport` | Hollow Knight |
| `oddmar_input` | Oddmar |
| `samurai2_offline` | Samurai II |
| `terraria_autoname` | Terraria |
| `skul_pad` | Skul（README 标注 **archived**） |

## D. 独立 loader 工程（架构上已隔离）

`projects/teapotloader`、`projects/limboloader`、`projects/hexagonloader` —— 各自独立可执行，含自己的 `javastubs/{teapot,limbo}.h/cpp`、`binding.cpp`、`com.playdead.limbo.LimboActivity` 等，不污染 `unityloader` 核心。

## E. 仅注释引用（留核心，属正当取证）

这些文件里的标题名只出现在注释中，说明"该通用修复以某标题为观测样本"，**不应改动**。

| 文件 | 命中 | 性质 |
|---|---|---|
| `projects/unityloader/javastubs/unity.cpp` | 11 | jnivm/反射通用修复 |
| `projects/unityloader/javastubs/unity.h` | 3 | 同上 |
| `projects/unityloader/javastubs/fakefmod.cpp` | 2 | FMOD 通用兜底 |
| `javastubs/javac.cpp` / `javac.h` | 5 / 1 | 同上 |
| `javastubs/android_content.cpp` / `bd_video.cpp` | 1 / 1 | 同上 |
| `libjnivm/src/jnivm/{internal/findclass,internal/method,vm}.cpp` | 3 / 1 / 1 | jnivm 通用陷阱 |
| `thunks/opensles/opensles.cpp` | 2 | 采样率通用适配 |
| `thunks/ndk/media.cpp`、`thunks/zlib/zlib.cpp` | 1 / 1 | 通用 |

另有一处**纯装饰性残留**（非标题逻辑）：`thunks/egl_sdl/egl_sdl.cpp:548` 的 `SDL_CreateWindow("Teapot", ...)`，NDK sample 遗留窗口标题。

## 统计与依赖

| 指标 | 值 |
|---|---|
| 核心内待抽**代码**行数 | 约 **690 行**（A2 ~190 + A3 ~500；A1 已抽入插件） |
| 需新增插件 | 2 个：`oddmar_video`、`oddmar_assetlocator`（`oddmar_input` 已完成） |

```text
oddmar_assetlocator (com.mobge.*)
        │  bd_video_note_asset_source()
        ▼
oddmar_video (libunity+0x4c124c 翻译)
        │  gamedata/assets/Videos/*.mp4
        ▼
通用视频链路 (javastubs/bd_video.cpp + thunks/khronos/gles2.cpp)   ← 留核心，无标题标记
```

## 插件 ABI 缺口（决定可行性）

| 要搬走什么 | 现状 | 缺口 |
|---|---|---|
| A2 `libunity+0x4c124c` | ABI v6 已提供模块查找/加载回调；`oddmar_video` 已迁移并在容器验证对非 il2cpp 模块装 RVA hook | 真机待充电后回归 |
| A3 `com.mobge.*` | 过渡实现仍在核心 `javastubs/bd_assetlocator.cpp`，由 `BD_ENABLE_MOBGE_ASSETLOCATOR` 控制；容器已验证目录枚举和 8 个 bundle reader | 跨 DSO 实验插件默认关闭；需宿主 C ABI 支持 byte array / object array / plugin-owned object / descriptor 注册 |
| A1 输入策略 | **已完成**：核心只广播 observer 事件；`oddmar_input` 插件持有策略和 RVA hooks | 容器/Xvfb 回放通过；TrimUI 真机已验证插件安装、A 键输入隔离与 `tryQuit` gate |
| （将来）GL/着色器怪癖 | 无 GL 钩子 | 需新增 GL hook 注册；**当前视频代码通用，不需要** |

### ABI v7（已落地）

| ABI 能力 | 用途 | 先后 |
|---|---|---|
| `find_module(name)` / `register_module_loaded(name, cb)` | **已实现**：A2 找 `libunity.so`，并在 `libunity` 载入后再装 Oddmar 视频 hook；若模块已存在，注册时会立即回调 | A2 已解锁 |
| `register_input_observer(observer)` | **已实现**：A1 从核心拿 SDL/controller 边沿、axis、frame begin | A1 已完成 |
| `jni_new_byte_array` / `jni_byte_array_write` | A3 返回 `GetBytes()` | 已实现 |
| `jni_new_string_array` / `jni_string_array_set` | A3 返回 `ListAssets()` | 已实现 |
| `jni_new_object` / `jni_object_userdata` | A3 返回真实 `AssetReader`，并让后续 instance method 找回 reader 状态 | 已实现 |

其中 A1 已完成核心清理：核心只广播原始输入与帧边界，`OddmarInputState`、gate 策略和 il2cpp RVA hook 均由 `oddmar_input` 插件持有。

## 建议落地顺序

1. **核心内务清理**（低风险，不改行为）：`BD_FORCE_VIDEO_TEXTURE`、`BD_SWALLOW_GL_ERROR` 已删除；诊断日志统一到 `BD_VIDEO_TRACE_SHADERS` 并默认静默；`egl_sdl` 对 `bd_dump_video_rt` 的注册式回调改造仍待单独实施。
2. **`oddmar_input` 插件**：**A1 已完成**：ABI v6 的通用输入观察者、`so_base` 已接入，`OddmarInputState`、输入 gate 与全部 Oddmar il2cpp RVA hooks 已从核心迁入插件。容器单测通过，启用插件的 Xvfb 回放运行约 12.5 秒、产生 3 个 GL 帧且无 `BD-SEGV`。本轮遵守用户约束，未进行真机测试或部署。
3. **`oddmar_video` 插件**：**A2 已完成并通过容器回放**：`libunity+0x4c124c` hook 由模块加载回调安装，splash 与 `game-start.m4v` 均解析到 `gamedata/assets` 的真实文件。
4. **`oddmar_assetlocator` 完整插件化**：**A3 已完成并通过容器回放**：核心关闭 `BD_ENABLE_MOBGE_ASSETLOCATOR`，插件实际执行 `ListAssets`、创建 `AssetReader`，并在回放中打开 bundle1 至 bundle7；无 `BD-SEGV`，无 `Unable to read header from archive file:`。
5. **回滚策略**：若真机发现插件 ABI 或时序回归，可暂时启用 `BD_ENABLE_MOBGE_ASSETLOCATOR=ON` 并移除 `oddmar_assetlocator.so`，核心过渡实现仍可独立工作。

## 目标布局

```text
projects/unityloader/plugins/
  oddmar_input/          # A1：RVA hook + 闸门策略（已完成）
  oddmar_video/          # A2：libunity+0x4c124c 契约（需 ABI 补模块访问）
  oddmar_assetlocator/   # A3：com.mobge.*（需 ABI 补 jnivm 对象/数组，或先走编译开关）
核心只保留：通用输入 observer、输入事件发射、视频/GL 通用实现
```
