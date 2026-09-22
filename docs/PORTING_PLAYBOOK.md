# Unity IL2CPP → Linux ARM 掌机移植 Playbook

面向 1GB 级 UMA 掌机（如 Anbernic BuildRoot）。新端口**先读本文**；大文件推送见根目录 [`AGENTS.md`](../AGENTS.md)。

## 0. 何时不要开坑（止损判据）

移植前用解包 + `tools/unity_astc/inventory_bundle.py` 粗估：

| 红灯 | 说明 |
|------|------|
| 标题/菜单阶段 Addressables 热包贴图+音频已近物理内存 | 进关再加载必顶穿 |
| 单个 bundle 同时塞「全角色/全音效/全 UI」 | 无法部分卸载；缩纹理只能延缓 |
| 运行时压 ASTC/ETC2 无效 | `textureMaxDim` **拦不住**压缩上传，必须离线 retier |

**负面案例：Skul（已止损）** — 标题预加载后 RSS≈785MB / `sys_avail`≈72MB，进关前就顶到内存墙。完整长文在 `LinuxArmPorts/SKULL_ARM_LINUX_PORTING.md`。

**正面案例：Maximus2** — 同一节里也记了它踩过的 `JNIVM_ENABLE_RETURN_NON_ZERO` 缓存坑（**先读 playbook §1.2**，能省几小时）。

## 1. 标准流水线

```text
APK/解包 → 本地 staging
    → inventory / astc_retier / shrink / audio_stream_patch
    → Docker：Release + 中间态日志（LOG=ON，TRACE/VERBOSE 关）编 unityloader + plugin
    → Dropbeak push (--force --chunk --chunk-size 16m --verify)
    → 掌机 Ports 手启（勿随意远程 .sh）
    → 看 log：退出码、prefs flush、`[BD-MEM]` / 视频桥关键行；深挖再开 TRACE
```

### 1.1 发布构建（小体积、关键日志）

日常上机用 **Release + 中间态日志 + strip**（约 5–7MB，而非 Debug ~90MB）。

日志为**编译期**开关（见 `platform/common/logging.h` / `AGENTS.md`）：

| 开关 | 上机（中间态） | 全关 / 深挖 |
|------|----------------|-------------|
| `BD_ENABLE_LOG` | **ON**（主开关：`[BD-MEM]` / `publish`/`swap` / codec 摘要 / worker / `[STUB-MISS]`） | OFF 压体积对照；深挖保持 ON |
| `BD_ENABLE_TRACE` | OFF（upload/step/luma/blit 等 `BD_DEBUG` 在此） | 深挖按需 ON |
| `BD_ENABLE_VERBOSE` | OFF | 仅深挖 ON（行数极多） |
| `CMAKE_BUILD_TYPE` | Release + strip | Debug 或 Release+TRACE |

中间态视频桥已把热路径降到 TRACE，掌机实测约 **6–7 行/s**；**不再继续精简 LOG**。
全关日志对照：同一目录改 `-DBD_ENABLE_LOG=OFF`；深挖再加 `-DBD_ENABLE_TRACE=ON`。toml `[debug] mem_log_interval_ms` 仅在 LOG 打开时有输出（且**可省略**，见 §3）。
```powershell
docker run --rm --platform linux/amd64 `
  -v "D:\Locke\gitee\Bogodroid:/work" -w /work/build-aarch64 `
  bogo-builder:unity2017-armv7 bash -c @"
cmake . -DCMAKE_BUILD_TYPE=Release \
  -DBD_ENABLE_LOG=ON -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF \
  -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF
cmake --build . -j2 --target unityloader plugin_<name>
aarch64-linux-gnu-strip --strip-unneeded unityloader unityloader.d/*.so
"@
```

### 1.2 `JNIVM_ENABLE_RETURN_NON_ZERO`：必须显式 OFF

`libjnivm` 的这个选项决定**缺桩**（日志里的 `[STUB-MISS]`）返回什么：

| 值 | 缺桩返回 | 后果 |
|----|----------|------|
| **OFF**（默认，上机必须） | `null` / 0 | Unity 自己 try/catch 掉，最好情况只丢一个 NRE |
| ON（实验） | 硬造一个 dummy 对象 | Unity 把它 cast 成 `String` / `Throwable` → jnivm `Invalid Reference, Unexpected Type` → `terminate()` / `exited 134` |

它是 **CMake 缓存项**，不是源码常量：共用 `build-aarch64/` 时，只要有一次实验把它设成 `ON`，之后即使只改无关代码，编出来的 `unityloader` 也会崩，且崩点看起来发生在"完全不相干"的桩上（实测踩过：`File.getParent`、`Context.getContentResolver`、`java/lang/Error`）。

**所以每次构建都显式写 `-DJNIVM_ENABLE_RETURN_NON_ZERO=OFF`**，编完核对：

```powershell
Select-String -Path build-aarch64\CMakeCache.txt -Pattern JNIVM_ENABLE_RETURN_NON_ZERO
```

（Ninja 构建下想再确认编译命令里没带宏，可在容器里 `ninja -t commands | grep -c JNI_RETURN_NON_ZERO`，应为 `0`。）

判读崩溃日志时先分清：`terminate called recursively` / `exited (134)` + `Invalid Reference, Unexpected Type` = 本开关被打开的症状，**不要**顺着最后一个 `STUB-MISS` 去补桩；`[BD-SEGV]` + libunity 地址才是游戏内部异常。Maximus2 就是踩这个坑的现场：它是在"只加了分辨率探测"的构建上开始崩的，三次日志的最后一个 `[STUB-MISS]` 各不相同，照着补桩全部无效。

### 1.3 其它 `JNIVM_*` 缓存项：`FORCE` 与各自的正确取值

`JNIVM_ENABLE_RETURN_NON_ZERO` 不是孤例：`libjnivm` 的开关全是 **CMake 缓存项**，`set(... CACHE BOOL ...)` 不带 `FORCE` 时，上一次配置遗留的旧值会静默存活（这正是 §1.2 那类崩溃的来源）。`CMakeLists.txt` 现在把这两项 `FORCE` 成固定值：

| 开关 | 取值 | 原因 |
|------|------|------|
| `JNIVM_ENABLE_DEBUG` | **恒 ON** | **不是日志开关**。它即 `JNI_DEBUG`：包住 `InternalFindClass()` 里的命名空间/类注册遍历（影响嵌套类如 `Foo$Bar` 的类身份与 `nativeprefix`），并保留 `CallMethod/GetField object is null` 诊断。所有上机验证过的构建都是 ON，未上机验证前别改 |
| `JNIVM_ENABLE_TRACE` | 跟随 `BD_ENABLE_LOG` | 即 `JNI_TRACE`，只加 `LOG()`。但 `BD_ENABLE_LOG=OFF` 时这些块里除了死日志，还有**未知字段路径**上的 `Util::GetClass()`（非日志调用，每次都会执行），所以 LOG 关时一并关掉 |

核对缓存（`FORCE` 后应等于上表）：

```powershell
Select-String -Path build-aarch64\CMakeCache.txt -Pattern 'JNIVM_ENABLE|BD_ENABLE_LOG'
```

一个容易踩的交互：`cmake --build` 发现 `CMakeLists.txt` 比 `build.ninja` 新时会**自己重跑 configure**，用的是缓存里的值。所以"只改 CMakeLists、命令里不带 `-D`"得到的是**旧缓存 + 新建模规则**的混合结果；要确定，先显式跑一次 §1.1 的 `cmake -S . -B build-aarch64 -D...`，或删掉 build 目录重配。

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
| 进程内存 | `[debug] mem_log_interval_ms`；**可省略**（默认 2000 ms），`BD_MEM_LOG_MS` 环境变量优先，`0` 关闭；需 `BD_ENABLE_LOG`（关掉时整段编译移除，不再每 2 s 读 `/proc`）；标签 `[BD-MEM]` |
| 分辨率 / 刷新率自动探测 | `[device] displayWidth/Height/RefreshRate`；`0` 或省略 = 探测（SDL → `/dev/fb0` → 640x480@60）。实现 `platform/common/device_display.cpp`，日志标签 `[BD-DEVICE]` |
| PAD Java stub | `[play_asset_delivery] enabled/default_pack/pack_version/pack_path_template` |
| 每帧 present 钩子 | Plugin ABI v3：`register_present_callback` |
| CPU present / swap pause | `BD_EGL_CPU_PRESENT`、`BD_EGL_SWAP_PAUSE`（插件可 setenv） |

一份 TOML 适配多机型：`[device]` 的三项留 `0` 即可跟随面板；只有需要 letterbox / 逻辑分辨率与物理面板不同时才写死。JNI 桩（`Display`、`DisplayMode`、`ANativeWindow`、`eglQuerySurface`、纹理上传判定）全部读同一份探测结果，勿再各自查 toml。

配置项一律**可省略**：`[device]` 的 `displayWidth/Height/RefreshRate` 省略即探测，`[debug] mem_log_interval_ms` 省略即 2000 ms（整个 `[debug]` 表都可以不写）。给掌机铺配置时先只写必要项，别把默认值抄进去。

标题专属逻辑放 `unityloader.d/<game>.so`。

## 4. 插件约定

1. `abi_version == BOGODROID_PLUGIN_ABI_VERSION`（当前 **3**）。
2. 每帧工作用 `register_present_callback`，不要让核心 `dlsym` 游戏名符号。
3. PAD 路径补丁 / IL2CPP hook 留在插件。

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
