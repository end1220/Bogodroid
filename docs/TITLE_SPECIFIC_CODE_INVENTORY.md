# 标题专属代码清单（Title-Specific Code Inventory）

本文件盘点 `oddmar` 分支树里**所有标题专属（title-specific）代码与非代码资产**，用于判断哪些该留在核心、哪些该抽进插件。

## 扫描口径

| 项 | 值 |
|---|---|
| 分支 / HEAD | `oddmar` @ `810a630` |
| 文件类型 | `*.cpp *.h *.c *.hpp *.toml *.sh *.py *.md *.json` |
| 排除 | `_trimui_logs/`、`build*/`、`libjnivm/{tests,examples}`、`tomlplusplus/` |
| 判定 | 严格区分**代码命中**与**仅注释命中**；仅注释引用不算"专属代码" |

标题标记词：`oddmar`、`mobge`、`senri`、`hollow knight`、`skul`、`samurai`、`madfinger`、`maximus`、`terraria`、`fivehearts`、`heishenhua`、`limbo`、`playdead`、`hexagon`、`teapot`。

> ⚠️ 本清单**只覆盖当前分支树**，不构成 154 个提交的逐条归属核定。另外 `main` / `unity6` 与本分支**无共同祖先**（`git merge-base` 为空），因此不能用 `main` 做差异基线。

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
| `platform/common/oddmar_input_state.h` | `OddmarInputState` 类，**整文件标题专属** | 97 |
| `platform/common/input_backend.cpp:62-186` | `namespace bd_oddmar_input_gate` 全部策略 | ~125 |
| `platform/common/input_backend.h:16-18` | gate 对外声明 | 3 |
| `projects/unityloader/main.cpp:101-108` | `g_oddmar_*_orig` 原始函数指针 | 8 |
| `projects/unityloader/main.cpp:109-259` | 7 个 hook（pause/quit/jump/attack/attack2/walk/raw_button）+ 2 个 UI 模板 | ~150 |
| `projects/unityloader/main.cpp:1569-1621` | 安装块：硬编码 il2cpp RVA + 包名判定 | ~53 |

硬编码的 guest 偏移（全部指向 Oddmar 的 `libil2cpp.so`）：

| `main.cpp` 行 | 符号 | RVA |
|---|---|---|
| 1573 | `pause_toggle` | `0x93047C` |
| 1574 | `try_quit` | `0x8E53AC` |
| 1582 | `ui_rvas[]` | `0x948BF8` `0x948E04` `0x97D5F0` `0xFD4C48` `0xFD4C68` |
| 1592 | `jump` | `0x9483A8` |
| 1593 | `raw_button` | `0x17EB88C` |
| 1594 | `attack` | `0x9485CC` |
| 1595 | `attack2` | `0x9487E8` |
| 1596 | `walk` | `0x94DD00` |

配置键：`[input] oddmar_menu_gate`、`[input] oddmar_sdl_buttons`。
包名门控：`packageName == "com.mobge.Oddmar"`（`main.cpp:1572`）。

核心 → gate 的回调点（抽插件后应改为**通用输入观察者**）：`input_backend.cpp:862,1058,1073`；`main.cpp:1830,1842`。

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
| `javastubs/bd_assetlocator.cpp` | `AssetLocator` + `AssetReader` 全部实现 | 384 |
| `javastubs/android.h:1127-1240` | 两个 jnivm 类声明 + `DEFINE_CLASS_NAME("com/mobge/...")` | ~114 |
| `javastubs/android_descriptors.cpp:107-115` | `registerClass<>()` 注册 | 9 |
| `CMakeLists.txt:119` | 无条件编入核心 | 1 |

`com.mobge.*` 是发行商私有包，非 Android/Google 标准包。同类问题核心已有先例：`com.google.*` 走编译开关 `BD_ENABLE_GPLAY`（`CMakeLists.txt:297-302`）。

### A4. 隐藏耦合：A2 ↔ A3 必须同进同出

`javastubs/bd_assetlocator.cpp` 反向调用 `main.cpp` 的 `bd_video_note_asset_source`：

| 位置 | 角色 |
|---|---|
| `main.cpp:311` | 定义 `extern "C" void bd_video_note_asset_source(const char*)` |
| `javastubs/bd_assetlocator.cpp:38` | 前置声明 |
| `javastubs/bd_assetlocator.cpp:154` | 调用 `bd_video_note_asset_source(file.string().c_str())` |

即 **A2 与 A3 是同一套 Oddmar 私有协议的上下游**，拆一个必须同时拆另一个。

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

## C. 已抽出的插件（合规）

`projects/unityloader/plugins/`：

| 插件 | 标题 |
|---|---|
| `hollow_knight_viewport` | Hollow Knight |
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
| 核心内待抽**代码**行数 | 约 **1,100 行**（A1 ~530 + A2 ~190 + A3 ~500） |
| 需新增插件 | 3 个：`oddmar_input`、`oddmar_video`、`oddmar_assetlocator` |

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
| A2 `libunity+0x4c124c` | 插件只拿到 `api->il2cpp`；且 `plugin_host::load(&lil2cpp,…)` 在 `main.cpp:1624`，`libunity` 到 `main.cpp:1639` 才载入 | ① 按名取模块（或暴露 libunity base）；② libunity 之后的第三阶段 / "模块就绪"回调 |
| A3 `com.mobge.*` | `register_jni_class` 只收 C 回调，不能返回 jnivm 对象，无 `BEGIN_NATIVE_DESCRIPTOR` | 扩 ABI 暴露 jnivm 类注册；过渡期先用编译开关 |
| A1 输入策略 | 核心 `input_backend.cpp` 直接 `#include "oddmar_input_state.h"` | 需通用"输入观察者"注册点：核心只报事件、插件定策略 |
| （将来）GL/着色器怪癖 | 无 GL 钩子 | 需新增 GL hook 注册；**当前视频代码通用，不需要** |

## 建议落地顺序

1. **核心内务清理**（零风险，不改行为）：删死开关 `BD_FORCE_VIDEO_TEXTURE`、`BD_SWALLOW_GL_ERROR`；诊断日志统一到 `BD_VIDEO_TRACE_SHADERS` 并默认静默；`egl_sdl` 对 `bd_dump_video_rt` 的反向依赖改为注册式回调。
2. **`oddmar_input` 插件**：ABI 已足够，只需在核心留通用输入观察者接口。
3. **补 ABI**（模块按名查找 + 阶段回调），再迁 `oddmar_video`。
4. **`oddmar_assetlocator`**：先编译开关过渡，等 ABI 支持 jnivm 类注册再进插件。

## 目标布局

```text
projects/unityloader/plugins/
  oddmar_input/          # A1：RVA hook + 闸门策略（ABI 已够，最易落地）
  oddmar_video/          # A2：libunity+0x4c124c 契约（需 ABI 补模块访问）
  oddmar_assetlocator/   # A3：com.mobge.*（需 ABI 补 jnivm 类，或先走编译开关）
核心只保留：通用 gate 接口、输入事件发射、视频/GL 通用实现
```
