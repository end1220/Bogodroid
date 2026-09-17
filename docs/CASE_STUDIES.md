# 案例集

新端口先读 [`PORTING_PLAYBOOK.md`](PORTING_PLAYBOOK.md)。这里只留结论与判据，长文分别在 `LinuxArmPorts/` 下。

---

## 案例一：Skul（小骨）— **止损**

完整长文仍在 `LinuxArmPorts/SKULL_ARM_LINUX_PORTING.md`。

### 结论

- Unity 2020.3 IL2CPP + URP + Addressables/PAD；主界面可玩，**进关失败**。
- 1GB UMA：标题预加载后 RSS≈785MB / `sys_avail`≈72MB；`gameBase` 后 ≈821MB / ≈7MB → 假死或 137。
- 根因：**bundle 划分过粗**（热包同时塞 UI/角色/音频），不是单点 stub 能修。
- **已止损**；`skul_pad` 仅归档，默认 `enabled=false`（示例配置 `configs/skul.toml` 已删，需要时按插件 README 现写这几行）。

### 可复用到核心的能力

| 能力 | 位置 |
|------|------|
| `[BD-MEM]` RSS / sys_avail | `platform/common/process_memory.h` |
| PAD Java stub 配置化 | `[play_asset_delivery]` + `javastubs/unity.cpp` |
| present 插件回调 ABI v3 | `register_present_callback` |
| 资产工具 | `tools/unity_astc/`（inventory / ASTC / audio_stream `--allow-embedded`） |
| 大文件推送 | 根目录 `AGENTS.md`（Dropbeak `--chunk --chunk-size 16m --verify`） |

### 踩坑清单（通用）

1. **`textureMaxDim` 压不了 ASTC/ETC2 上传** → 必须离线 `astc_retier` / `shrink`。
2. UnityPy 写回后**磁盘常变大**；以运行时 RSS 为准。
3. Addressables 内嵌音频：`audio_stream_patch --allow-embedded`；LoadType 改不动标题贴图峰值。
4. 根分区 ext4 **可开 swap**，TF vfat 不行；swap 往往把 OOM 变成更久假死，不作可玩方案。
5. Allwinner mali-fbdev：部分场景切换用 `BD_EGL_CPU_PRESENT` + 插件 present 轮询，勿在核心写死游戏名。
6. 覆盖运行中二进制可用 `--force`；勿远程乱启游戏 `.sh`（除非明确要求）。

### MEM 样点（止损证据）

| 阶段 | RSS | sys_avail |
|------|-----|-----------|
| Main | 246 MB | 617 MB |
| Base | 334 MB | 529 MB |
| 标题加载中 | →784 MB | →72 MB |
| gameBase 后 | →821 MB | →7 MB |

---

## 案例二：Maximus2（街头角斗士 2）— **跑通**

| 项 | 值 |
|----|-----|
| 包名 / 引擎 | `com.FourFats.Maximus2` / Unity **2020.3.23f1** IL2CPP arm64 |
| 掌机 | Anbernic，Mali-G31，面板 640x480，`172.16.7.55` |
| 远端 | `/mnt/mmc/Roms/PORTS/Maximus2/` |
| 本地 staging | `LinuxArmPorts\maximus2_port_stage\Maximus2\` |
| 资产 | `data.unity3d` 88.8MB → **75.9MB**（`astc_retier --include-compressed --keep-cap 512 --block 8x8` + `audio_stream_patch --mode hybrid`） |

### 最大的一课：崩因不是你的改动，而是 CMake 缓存里的 `JNIVM_ENABLE_RETURN_NON_ZERO`

现象：`Maximus2` 在"只加了分辨率探测"的构建上开始崩，`exited (134)`。3 次日志的最后一个 `[STUB-MISS]` 各不相同：

| 顺序 | 日志末尾 | 误判 |
|------|----------|------|
| 1 | `File.getParent` → `Expected N5jnivm6StringE` | 补 `File.getParent/getParentFile/getAbsolutePath/getName` |
| 2 | `Bundle.getString` / `getContentResolver` → `Expected N5jnivm9ThrowableE` | 补 `Bundle.getString(key)`、`getContentResolver` |
| 3 | `java/lang/Error.setStackTrace` → `[BD-SEGV]` | 补 `java/lang/Error`、`Exception`（继承 `jnivm::Throwable`） |

真相：`build-aarch64/CMakeCache.txt` 里 `JNIVM_ENABLE_RETURN_NON_ZERO:BOOL=ON` —— 是**上一次 PC01 排查**把它打开的，而"不崩的那份" Maximus2 loader 是更早、默认 `OFF` 时编的。于是本次"只想测分辨率"的构建实际同时翻了两件事：开关 `OFF→ON` ＋ 新桩。`ON` 让每个 `[STUB-MISS]` 都吐 dummy 对象，Unity 拿去 cast 就炸。

**判据**：`terminate called recursively` / `exited (134)` + `Invalid Reference, Unexpected Type` = 开关被打开，**不要去追最后一个 `STUB-MISS`**。改回 `OFF` 后同一个游戏直接跑通。详见 [`PORTING_PLAYBOOK.md`](PORTING_PLAYBOOK.md) §1.2。

### 顺带确认下来的通用结论

1. **一份 TOML 适配多机型可行**：`[device] displayWidth/Height/RefreshRate` 留 `0` 即自动探测（SDL → `/dev/fb0` → 640x480@60），日志 `[BD-DEVICE]` 能看到解析来源。实测序列：JNI 桩先问 → `640x480 (fb0)`，`SDL_Init(VIDEO)` 后再探 → 面板真实值 `(sdl)`。
2. **诊断期保留的桩仍有价值**（`File` 路径族、`Bundle` 取值族、`Context.getContentResolver`、`List/ArrayList.isEmpty`、`ArrayList.add` 注册名修正）：它们让缺桩行为更接近真 Android，而不是依赖"返回 null 也不要紧"。
3. `ArrayList::add` 曾被注册成 `"size"`（真 bug），已修。

---

## 案例三：FiveHearts — `textureMaxDim` 把 **RenderTexture** 也缩小了（画面被裁到左下）

**现象**：掌机上（640x480）视频"全屏显示原视频的左下部分"；同一份资源在 Unity 编辑器
和 Android 真机上完全正常。UI 本身没错（Skip 按钮照旧落在右下角）。

**根因**：`[gpu] textureMaxDim = 512`。缩放挂在 `bd_glTexStorage2D()`
（`thunks/khronos/gles2.cpp`），命中的 RGBA8 纹理等比缩到长边 512。它只豁免两类：

- `short_side <= 32` 或 `long_side > 4 * short_side`（LUT / 条带）；
- **尺寸正好等于屏幕**（`width == displayWidth && height == displayHeight`）。

游戏 `VideoPlaybackManager` 建的是 `new RenderTexture(1280, 720, 0, ARGB32)`：
1280x720 ≠ 屏幕 640x480 → **不豁免** → 实际分配 **512x288**
（1280 × 512/1280 = 512、720 × 512/1280 = 288，比值 0.4）。
Unity 自己的 `glViewport` 仍是 `[0 0 1280 720]`，于是那段渲染只有四边形的
**左下 40% × 40%** 落进 RT，UI 再把它铺满屏幕 → 看上去就是「视频只有左下那一块、放大 2.5 倍」。

**判据（一眼可辨）**：日志里
`video blit target attachment 0 is object 25 size=512x288 viewport=[0 0 1280 720]`
—— **attachment 尺寸 ≠ 它自己的 viewport**，且比值恰好是 `textureMaxDim / 长边`。
启动日志的 `[BD-CAP] textureMaxDim RGBA8=512` 就是开关状态。

**验证结果（2026-09-16，FiveHearts 掌机实测）**：`unity.toml` 里
`textureMaxDim = 0` → 重跑，同一行日志变成
`attachment 0 is object 25 size=1280x720 viewport=[0 0 1280 720]`（尺寸与 viewport 一致），
**画面完整**。峰值 `rss≈402MB`，与 cap 开着时的历史运行（330~430MB）同一量级，
所以「关掉 cap」是可用方案；要保住省内存收益则需让 cap 认得 RT。

**通用教训**：

1. **`textureMaxDim` 会打在 RenderTexture 上**。RT 是"运行时会画进去的面"，缩小它
   等于让 `glViewport` 与 FBO 尺寸脱钩 → 画面被裁、还很难和"UI 布局错"区分开。
   受影响的不止视频：任何 > 上限的后处理 / 剧情背景 / 片尾 RT 都会被同样裁掉。
2. **「尺寸正好等于屏幕」这条豁免不够用**：游戏 RT 常见尺寸是
   1280x720 / 1920x1080 / 半分辨率，正好都不等于屏幕尺寸。
3. 排查顺序：**先看 FBO attachment 的真实尺寸与 viewport 是否一致**，再去怀疑
   shader、UV、RawImage 布局。这次的 512x288 一度被当成"量错了的假值"，
   白白多绕了一轮。
4. 临时验证用配置即可：`[gpu] textureMaxDim = 0`（或只关 `textureMaxDimRGBA8`）。
   正经修法是让 cap **认得渲染目标**（延迟到明确是内容上传再缩，或被
   `glFramebufferTexture2D` 命中的纹理不缩）。
5. 版本跨度上值得注意：本 case 是 Unity **2022.3**（走 `glTexStorage2D` 的不可变 storage），
   Skul / Maximus2 是 2020.3 —— 老的 `glTexImage2D` 路径 `textureMaxDim` 同样是隐患，
   只是当时没撞上 RT。
