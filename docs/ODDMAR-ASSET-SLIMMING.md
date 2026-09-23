# Oddmar 资源包瘦身方案（视频 / 贴图）

> 日期：2026-09-22 深夜 · 分支 `oddmar` · 状态：**方案 + ✅ 已执行并通过验收**（§9）
> 前置阅读：[`ODDMAR.md`](ODDMAR.md) §0（现状）、§⛔（接手第 0 步）、§8（诊断开关）、§9.2（跑测命令）。
> 目标机：Anbernic 掌机 —— **物理面板 640×480**、Mali-G31、GLES 3.2（`ANBERNIC.md` §1）。
> 本文所有数字都是**实测**：脚本在 `D:\Locke\gitee\LinuxArmPorts\oddmar_port_stage\_analysis\`，命令见 §7。

---

## 0. TL;DR

| 项 | 现状 | 目标 | 净收益 | 风险 |
|---|---|---|---|---|
| **视频** | 19 个文件、**全部 1920×1080**、232 MB（+内嵌 12 MB） | **854×480** / H.264 Main@3.1 / capped-CRF23 / `-maxrate 700k` / `-refs 2 -bf 2` | ✅ **232.46 → 132.83 MB（−42.9%）**；解码像素 **−5.06×**（每帧 3 110 400 → 614 880 B） | ✅ 已做（hook 已修，见 §2.4） |
| **贴图** | 425 张、**约 98 MB** VRAM 估算（ETC2 58 + RGBA32 29） | ASTC **RGBA** 8×8（>128 px、非字体/非内建） | ✅ **22 容器实测：磁盘 74.83 → 31.96 MB（−42.87 MB）；纹理 VRAM 140.9 → 36.9 MB（−104.0 MB，−73.8%）** | ✅ 已做（工具两个缺陷已修，见 §3.1；分片文件陷阱见 §9.3） |
| **运行时** | `[BD-MEM] hwm` **704.8 MB**（原始资源） | — | ✅ 同条件对照 **672.7 MB（−32.1 MB）** | — |
| 合计 | — | — | 磁盘 **−143 MB**、纹理显存 **−104 MB**、过场解码/上传 **−5.06×** | — |

**一句话**：视频是这台机器上**单项最贵的资源开销**（软解 + 按源分辨率上传 1080p，每帧 3.11 MB），
其次是贴图里那 29 MB 未压缩 RGBA32 和 58 MB ETC2_4×4；两项一起做，收益最大、改动面最小。
**音频 158 MB 是比这两项都大的一块，但属于另案**（§5）。

---

## 1. 现状实测（证据）

### 1.1 资源分布（`gamedata/assets` 565 MB）

| 目录 | 大小 | 内容 | 本次处理 |
|---|---|---|---|
| `Videos/` | 232 MB | 18 个 `.m4v` + splash `.mp4`，**全部 1080p** | ✅ §2 |
| `SoundBanks/` + `Audio/` | 158 MB | Wwise `.bnk` / `.wem` | ⏸ §5（Wwise 初始化**已修好**；但 bank 是**运行时按需加载**，标题界面 `LoadBank`=0，是否死重测不出） |
| `bin/Data/` | 128 MB | 713 个文件：Unity 标准数据 + 700+ 个**裸 SerializedFile**（MD5 命名，一个资源一个文件） | ✅ 其中的 191 张贴图 |
| `Bundles/` | 38 MB | `bundle1..8`（UnityFS），231 张贴图 | ✅ §3 |
| `RawAssets/` | 11 MB | `StoryVideos/game-start.m4v`（1080p 9.9 MB）+ 多语言 `.srt` | ✅ §2（视频）；字幕可选裁 |
| 其它 | ~10 MB | `PredownloadedAssets`、`VikingMushroom`、`desc.txt` | ⚪ |

> **容器判定**：`Bundles/*` 是 `UnityFS` 包；`bin/Data/<md5>` 是**裸 SerializedFile**（`assets/bin/Data/split0..5`、
> `globalgamemanagers`、`level0` 是标准命名，其余 700 个是 MD5 命名）。
> ⚠️ 这决定了 `astc_retier.py` 必须能处理 SerializedFile —— 见 §3.1-A。

### 1.2 视频（19 个 + 1 个内嵌）

| 文件组 | 分辨率 | 帧率 | 时长 | 大小 | 码率 |
|---|---|---|---|---|---|
| `Videos/W*.m4v` × 18 | 1920×1080 H.264 **High** | 24/25 | 1497 s | **230.6 MB** | 1.29 Mbps（视频 1.16 + 音频 0.13） |
| `mobge_and_senri_splash_video.mp4` | 1920×1080 | **60** | 4.23 s | 0.98 MB | 1.85 Mbps |
| `RawAssets/StoryVideos/game-start.m4v` | 1920×1080 | 24 | 54.4 s | 9.88 MB | 1.45 Mbps |
| `bin/Data/d8db21c482742174d89e0880ded078d0.resource`（`VideoClip unlockAllLevels`） | 1280×720 | 60 | 29.8 s | 12.10 MB | — |

**为什么视频是第一优先级**（四条独立证据）：

1. **解码是纯软件**：`thunks/ndk/media.cpp` 用 **FFmpeg**（`avcodec_send_packet` / `libavcodec`）实现
   `AMediaExtractor` / `AMediaCodec`，没有硬解。
2. **H700 硬解是 No-Go**（[`H700_VIDEO_DECODE_SPIKE.md`](H700_VIDEO_DECODE_SPIKE.md)，2026-09-17）：
   有 `/dev/cedar_dev` 但缺 aarch64 CedarX、缺 V4L2 M2M 解码节点 →
   **只能靠软解**，且那篇实测 **854×480 走同一套 `yuv_gpu` 通路只有 23–28 fps**。
   → 本包过场是 **1920×1080**（854×480 的 **5.06×** 像素）⇒ 现状**必定跑不动**。
3. **按源分辨率上传**：`unity.toml [video] path="yuv_gpu"`；日志 `[BD-VIDEO] drawable size 640x480
   (frames upload at source resolution)` → 1080p 帧 **3.11 MB/帧**（`[BD-MEDIA] decoded frame=0 … bytes=3110400`），
   24 fps 即 **75 MB/s** 的 YUV 搬运 + 每帧一次 GL 色彩转换；面板只有 640×480（0.41 MB/帧）。
4. **实测解不动**：一轮 30–60 s 的跑测只解出 **48 条** `decoded frame=`；splash 是 **1080p@60fps**
   —— 4×A53 软解最坏形态。`[BD-VIDEO] swap: … uploaded=19 frames=20/18`。

### 1.3 贴图（425 张 / 约 98 MB）

口径：**按格式/尺寸/米普数算出的驻留显存估算**（不是磁盘体积 —— UnityPy 重写后磁盘常变大，
`AGENTS.md` 已注明以运行时 `[BD-MEM] rss` 为准）。

| 格式 | 张数 | 现状 | 说明 |
|---|---|---|---|
| `ETC2_RGBA8`（4×4，0.5 B/px） | 318 | **58.0 MB** | 主力；ASTC 8×8 后 29.3 MB（−2×） |
| `RGBA32`（未压缩，4 B/px） | 40 | **29.2 MB** | 最划算：`SILAH_KESFETME_EFEK` 8.08 MB、`skeleton` 8.00 MB、`joypad tutorial` 4.00 MB、`titles` 2.62 MB；ASTC 8×8 后总计 1.9 MB（−16×） |
| `Alpha8`（`Font Texture` 2048²） | 5 | 8.1 MB | **必须保持**（字体图集） |
| `ETC_RGB4` | 18 | 1.5 MB | → 0.7 MB |
| `PVRTC_RGBA4`（`character test` 1024²） | 1 | 0.5 MB | Mali 上 PVRTC 是驱动模拟；转 ASTC 兼得体积与采样速度 |
| `ASTC_RGBA_4x4`（`blue_ground` / `earth1v2`） | 2 | 0.5 MB | **游戏原厂自带** → ASTC 可用性硬证据；已是最优，保持 |
| 其它小图 | 41 | 0.5 MB | 保持或低收益 |

**关键结构事实**：
- **402/425（95%）的贴图带 alpha** —— 这直接决定 ASTC 必须用 **RGBA** 变体，见 §3.1-B。
- **没有一张贴图使用外部流数据**（`m_StreamData.path` 全为空）→ 不需要碰 `.resS` / `.resource` 偏移，
  `--compact` 在这套资源上无用。
- 只有 **58 张**能被 Sprite 直接引用到（同文件路径级），`bin/Data` 之间还有**跨文件 PPtr**，
  静态判不全 → 任何"改分辨率"的动作都必须用 `shrink_bundle.py` 那一类会重写 Sprite rect 的路径（§3.4 可选）。

### 1.4 运行时基线（用于验收对比）

| 指标 | 基线值 | 出处 |
|---|---|---|
| RSS 峰值 | **807.8 MB**（稳态 ~780 MB） | `[BD-MEM] … hwm=807.8MB @ eglSwapBuffers` |
| 日志行数 | ~17.7 万行（哨兵态只有 ~1350 行） | `docs/ODDMAR.md` §⛔ |
| 画面判据 | X 根窗口截图 **290–300 KB**（≥180 KB 算活）；`seq/timeline.txt` 不能只有 192 B | §0.1 |
| 容器内存 | 15.5 GB / 32 核 → **容器里永远看不到 OOM**，判活只能靠计数与截图 | `free -m` |
| 容器 GL | `llvmpipe (LLVM 12.0.0)`，Mesa 21.2.6 | 日志 / `libgl1-mesa-dri 21.2.6` |
| 设备 GL | **GLES 3.2 / Mali-G31** → **ASTC LDR 是 ES 3.2 的强制项** | `ANBERNIC.md` §1 |

---

## 2. 计划 A：视频转码（先做，收益最大）

### 2.1 目标参数

面板是 **640×480（4:3）**，过场是 **16:9**。两个候选：

- **640×360**（推荐）：16:9 在面板宽度下的自然尺寸。854×480 会被缩到 640×360，
  多出来的 **1.78×** 解码/上传纯属浪费 —— 而 `H700_VIDEO_DECODE_SPIKE.md` 实测 854×480
  在这台机器上**只有 23–28 fps**，对 24 fps 内容属于**擦边**；降到 640×360 才留出余量。
- **854×480**：沿用 `H700_VIDEO_DECODE_SPIKE.md` 早先的推荐值（"离线转成 854×480@30、低参考帧"）。
  体积/解码都比 640×360 差，但画面余量更大（缩放更少）。**若担心 2D 手绘细节损失，选这个。**

```
ffmpeg -i <src> -vf scale=854:480:flags=lanczos \
  -c:v libx264 -profile:v main -level 3.1 -preset slow -tune animation \
  -crf 23 -maxrate 700k -bufsize 1400k -pix_fmt yuv420p -g 48 \
  -refs 2 -bf 2 \
  -c:a aac -b:a 96k -ac 2 -movflags +faststart <dst>
```

> ⚠️ **`-bf` 不是 `-bframes`。** ffmpeg 只认 `-bf`（`bframes=` 是 x264 私有选项，得走 `-x264-params`）。
> 写成 `-bframes` 会得到 `Unrecognized option 'bframes' / Error splitting the argument list`，
> **每一个文件都失败**，而脚本的 summary 只是个 `files: 0`。

- **保持 H.264 + AAC**：`AMediaExtractor` + FFmpeg 的现成通路，零适配。
- **High → Main/3.1**：关掉 8×8 变换，软解路径更短；`-tune animation` 对平色手绘动画性价比最高。
- **`-refs 2 -bf 2`**：**低参考帧** —— 直接缩小解码时的 DPB 与运动补偿开销，
  是 `H700_VIDEO_DECODE_SPIKE.md` 给的现成建议；相对 High profile 默认（ref 4/bframe 3）解码更省。
- **`-movflags +faststart`**：moov 前移，避免 extractor 反复 seek。
- **不改文件名/扩展名**：`gamedata/assets/Videos/*.m4v` 原地替换；长度由 loader `stat` 现取
  （`bypass_video_translate` 里逐路径 `bd_file_size()`），**文件变小不影响契约**。
- **帧率**：过场保持源帧率（24/25，本来就是动画原生帧率）；splash 是 1080p**60**，`-r 30` 后
  0.032 MB（对比 0.04 MB），logo 动画值得顺手砍一半解码量。

### 2.2 实测四档（全量 19 个文件 = 18 过场 + splash，全部带 `-refs 2 -bf 2`）

> **怎么读这张表**：体积由 **`-maxrate`** 决定，分辨率几乎不影响体积 ——
> 分辨率省的是**解码/上传的像素量**（每帧 I420 = `w×h×1.5`）。所以选档要看
> "同样的体积，解码负担差多少"。

| 档位 | 总大小 | 降幅 | 平均码率 | 每帧 I420 | 相对 1080p 解码 |
|---|---|---|---|---|---|
| 源（1920×1080） | 231.52 MB | — | 1.294 Mbps | **3.11 MB** | 1.00× |
| 1280×720 CRF22 / **1200k** | 214.10 MB | −7.5% | 1.196 Mbps | 1.32 MB | 0.44× |
| 1280×720 CRF23 / 800k | 151.87 MB | −34.4% | 0.849 Mbps | 1.32 MB | 0.44× |
| **854×480 CRF23 / 700k ← 选定** | **131.91 MB** | **−43.0%** | 0.737 Mbps | **0.59 MB** | **0.19×** |
| 640×360 CRF23 / 500k | 99.48 MB | −57.0% | 0.556 Mbps | 0.33 MB | 0.11× |

**两个反直觉的坑（都是实测）**

1. **CRF22 会把体积做大。** 第一版 1280×720 / CRF22 / maxrate 1200k 只省 7.5%，
   `W2L2end` 甚至 **164%**、`W1L5end` 126% —— 因为源 1080p 平均只有 1.294 Mbps，
   是**已经压得很狠**的源；CRF22 重编时 x264 试图保留更多细节，需要的码率反而超过源。
   **必须用 capped CRF**：显式给 `-maxrate`，让它真正约束住码率。
2. **720p 在这台机器上没有性价比。** 720p 比 854×480 多 20 MB 体积（+15%），
   却要解 **2.25×** 的像素。而 `H700_VIDEO_DECODE_SPIKE.md` 实测 854×480 只有 **23–28 fps**
   —— 720p 只会更差。屏幕最高 720p 是**上限**，不是必须铺满。

另加两项（已转）：

| 文件 | 原始 | 854×480 | 处理方式 |
|---|---|---|---|
| `RawAssets/StoryVideos/game-start.m4v` | 9.88 MB | **5.18 MB** | 原地替换（同 §2.1 参数） |
| `mobge_and_senri_splash_video.mp4` | 0.98 MB（**1080p60**） | **0.04 MB** | 原地替换（1080p60→854×480 收益极大） |

**总账**：232.46 MB → **132.83 MB**（−99.6 MB，−42.9%）。
内嵌的 `unlockAllLevels`（12.10 MB，见 §2.5）**本轮未做**，做了才是 ~123 MB。

### 2.3 步骤

1. **备份**：`Oddmar/gamedata/assets/Videos/` 整目录复制为 `Videos.orig/`（staging 侧，不进 git）。
2. **单样本验证**：先只转 `W3L3start.m4v`（最大，139.7 s）→ 覆盖进容器 `/game/Oddmar/gamedata/assets/Videos/` →
   按 §6 跑一轮 → 看 `[BD-MEDIA] decoded frame=… bytes=` 是否变成 **345600**、画面是否正常。
   样本不过，参数就别铺开。
3. **全量**：用 `encode_videos.py`（§7）批处理；产出一张 size 对照表留档。
4. **覆盖**：只覆盖 18 个 `.m4v` + splash mp4 + `game-start.m4v`；**不动** `RawAssets/**/*.srt`。
5. **核对**：容器内 `find … -printf '%s %p\n'` 逐文件比字节数，与 host 产出一致再算完。
6. **一轮跑测 + 截图**，与 §1.4 基线对比。

### 2.4 ✅ 已修：hook 原来把**所有**视频翻译指向 splash

**症状**：`bypass_video_translate` 只认一个写死的 splash 路径，**任何** URL 都被翻译成它
⇒ 18 个关卡过场从来没被真正播放过，也**因此重编码它们不会产生任何运行时效果**。

**根因（2026-09-22 才查清 —— 旧注释是错的）**

旧注释写 "*a1 里装着原始 URL，调用前被清空*" —— 后半句对，**前半句错**。
URL 根本不在我们收到的任何参数里。真正的消费者在 **`libunity+0x53f020`**，它把 URL 放在 **x23**：

```
53f020  str  x28, [sp, #-96]!
53f044  mov  x23, x2            ; x23 = const char* url   ← 原始 URL 在这里
53f07c  bl   strlen@plt
53f08c  bl   <string::assign>   ; 复制进 sp+0x480 那个局部 string ...
53f13c  bl   <string::assign>   ; ... 然后把它清掉 (null,0) ← 所以 a1 恒为空
53f150  bl   4c124c             ; ← 我们的 detour
```

`0x53f020`…`0x53f160` **全段反汇编确认：没有任何一条指令写 x23**。
所以 URL 在整个函数体里都活着，只是**不在我们的参数里**。

**修法**：hook 的**第一条语句**读 x23 —— x23 是 callee-saved，进入我们函数时仍是调用者的值：

```cpp
uintptr_t url_ptr = 0;
__asm__ volatile("mov %0, x23" : "=r"(url_ptr));   // 必须在最前面，后面任何调用都会用掉 x23
```

然后把 `assets/` 之后的部分当相对路径（`Videos/W1L1start.m4v`），拼 `<gamedata>/assets/` 得到本地文件，
**逐路径** `stat` 取长度（单个 `map`，不再是单值缓存）。解析不出或文件不存在 → **回落到 splash**，保证不退化。

> 路径指针必须在 hook 返回后仍然有效（Unity 是之后才 `open()` 的），所以路径存在
> `std::map` 的节点值里（node 稳定）或那个 static fallback 字符串里，**不能用局部 `std::string`**。

**验收**：日志应出现
`[BD-MEDIA] xlat: ok 'jar:file://!/assets/Videos/…' -> <本地路径> (<字节数>)`。

> ⚠️ **但标题界面只会请求 splash**（实测 `extractor new` = **2**，即 splash + 1 个）。
> **18 个关卡过场要到关卡里才会被请求**，所以"逐 URL 映射"这条只能在进关卡之后才能完整验收 ——
> 本轮能验的只有：映射逻辑本身跑通 + 日志里 URL 解析正确 + splash 正常播放。

### 2.5 内嵌视频（本轮**未做**，+9 MB）

`bin/Data/d8db21c482742174d89e0880ded078d0` 是 `VideoClip unlockAllLevels`，
`m_ExternalResources = {m_Source: d8db21c4….resource, m_Offset: 0, m_Size: 12097460}`；
那个 `.resource` 就是**裸 MP4**（`ftypisom…avc1`）。

做法：重编码 → 覆写 `d8db21c4….resource` → 用 UnityPy 把 `m_Size` 改写成新的字节数（该文件是裸 SerializedFile，
`env.file.save()` 已实测可用）→ 校验 `ffprobe` 能读出 29.8 s。**offset 恒为 0，只有一个条目，没有偏移要修**。
实测 1280×720 CRF23 → 2.91 MB（−76%）。

---

## 3. 计划 B：贴图 ASTC 8×8

### 3.1 ⚠️ 动手前必须修的两个工具缺陷（都在 `tools/unity_astc/astc_retier.py`）

**A. 撞裸 SerializedFile** —— `print_bundle_info()`（第 150 行）无脑取 `f.signature` / `f.dataflags` / `f.files`：

```
AttributeError: 'SerializedFile' object has no attribute 'signature'
```

Oddmar 有 **191 张贴图（33 MB）在 `bin/Data/<md5>` 里**，全是裸 SerializedFile。
修法：`print_bundle_info` 里先判容器类型，`BundleFile` 走原逻辑，`SerializedFile` 只打 header 版本；
`--compact` 分支同样要跳过（SerializedFile 没有 `files`，也没有 `.resS` —— 本资源实测 `m_StreamData` 全空）。

> 已用运行时打补丁的方式验证可行（不改仓库）：单个 1261×1261 RGBA32 文件
> **8.10 MB → 0.53 MB（−7.6 MB）**，输出能被 UnityPy 正常读回、尺寸/名字不变。

**B. 硬编码 `ASTC_RGB_*` 会**丢掉 alpha** —— 第 352 行：

```python
target = getattr(TF, f"ASTC_RGB_{bx}x{by}")     # 永远 RGB
```

- Oddmar **402/425 张贴图带 alpha**（2D 手绘 + 大量叠加特效，如 `alma_genis_isik*`、`vurma_efek`）；
- 游戏原厂自带的 ASTC 用的是 **`ASTC_RGBA_4x4`** —— 官方选择就是 RGBA；
- GLES 语义：按 **RGB** 内部格式采样时 **alpha 恒为 1.0** → 透明会变成不透明方块。
  （UnityPy 自己解码仍能读出 alpha 平面，所以这个错误**在离线校验里看不出来**，只有跑起来才看得见。）

修法：加 `--alpha-mode {auto,rgba,rgb}`，**默认 `auto`**：源格式带 alpha（`RGBA32/ETC2_RGBA/Alpha8/PVRTC_RGBA/…`）
→ `ASTC_RGBA_*`，否则 `ASTC_RGB_*`。ASTC 的 RGB/RGBA 变体**块大小相同（16 B）**，用 RGBA 不多花一个字节。

**C. 顺带**：`--block-small` / `--small-threshold` 也要走同一套 `alpha-mode` 解析（别只改主 `--block`）。
**D. 登记**：本字段 `m_MipCount` 实测在 spike 中从 11 变成 9（数据与声明自洽，但**变了**）→ 每轮抽 3 张
核对 `m_Width/Height/MipCount/TextureFormat`，写进验收记录。

### 3.2 分级策略

| 动作 | 条件 | 张数 | 收益 |
|---|---|---|---|
| **KEEP** | 内建（`unity default resources`、`Resources/unity_builtin_extra`）、字体（`Font Texture` / `* Atlas`）、`SpriteAtlasTexture-DebugLogUI`、长边 ≤128、已是 ASTC | 232 | 10.0 MB 不动 |
| **ASTC RGBA 8×8** | 长边 >128，其余全部 | 191 | 72.2 → 30.6 MB |
| （可选）downscale | 见 §3.4 | 2+ | 16.1 → 0.5 MB |

预估结果：**98.2 MB → 41.0 MB**。

> `--block-small` **不要开**：本资源的主体是 ETC2（0.5 B/px），ASTC 6×6 是 0.444 B/px —— **只省 11%**，
> 而 8×8 是 0.25 B/px（−2×）。6×6 只对 RGBA32 源有意义，而 RGBA32 大头（`SILAH…`、`skeleton`）用 8×8
> 已经是 −16×，没必要为它们单独降档。

### 3.3 步骤

1. **备份 staging**：`Oddmar/gamedata/assets/{Bundles,bin}` → `.orig` 副本。
2. **工具修完自检**：`--dry-run` 跑 `bundle7` + 一个裸 SerializedFile，确认跑通、`KEEP` 名单符合 §3.2。
3. **逐容器处理**（先 bundles 后 `bin/Data`）：
   `astc_retier.py <in> -o <in>.astc --include-raw --include-compressed --alpha-mode auto --block 8x8 --packer original --jobs 12`
   - `--include-raw`：覆盖 40 张 RGBA32（29 MB 的大头）；
   - `--include-compressed`：覆盖 ETC2/PVRTC（设备 Mali-G31 + GLES 3.2，ASTC 是强制项，已由 §1.3 的原厂 ASTC 佐证）；
   - `--packer original`：原样保留 `dataflags`（**绝不用 `lz4`**，UnityPy 的 lz4 预设会写 0x80 padding 位，
     2020.3 之前的 player 会 SIGBUS；本包是 2018.4，风险实打实）。
4. **skip 清单**（`--skip-file`）：`unity default resources`、`unity_builtin_extra`、`Font Texture`、
   `SpriteAtlasTexture-DebugLogUI (Group 1)-…`、`VikingMushroom/skip.png`。
5. **校验**：每个输出用 UnityPy 复读，抽 3 张比 §3.1-D 四项；再用 `inventory.py`（§7）重扫一遍，
   确认格式直方图落到 `ASTC_RGBA_8x8`、总量 ~41 MB、无 `load` 失败。
6. **同步进容器 + 跑测**（§6），**重点看透明**：标题界面、过场、菜单、粒子。

### 3.4 可选 Phase 2：降分辨率（先不做）

`back1` 4096×4096（ETC2，8 MB → ASTC 8×8 后 4 MB）：面板只有 640×480，理论上 1024²/2048² 就够。
但它是 Sprite 引用的，降分辨率必须同步改写 Sprite 的像素 rect —— 那是 `shrink_bundle.py` 的活
（同目录，README 已注明"会重写 Sprite rect、修不了 TMP 字形表"）。

**建议**：先做 §3.2（纯换格式、零元数据改动），跑测确认画面无碍后，再单独评估要不要动这几张。
`SILAH_KESFETME_EFEK`（1261×1261，mips 11）与 `skeleton`（2048×1024）实测**非 Sprite 引用**，
是降到 1024 的天然候选，但 ASTC 8×8 已经把这两张从 16.1 MB 打到 0.4 MB，继续降只多省 0.2 MB —— **不值得**。

---

## 4. 执行顺序与出口条件

| 阶段 | 动作 | 出口条件 |
|---|---|---|
| **P0** | 备份 `Videos/`、`Bundles/`、`bin/Data/`；确认容器 `unity.toml` 不是哨兵态（`grep android_package_code`） | 备份存在、toml 为 `"./"` |
| **P1a** | 修 `bypass_video_translate` 的逐 URL 映射（§2.4）+ 构建 + 跑一轮 | **能播出一个过场**；`ZZZSENTINEL=0`、`Unable to read header=0`、`could not translate=0`、截图 ≥180 KB |
| **P1b** | 视频单样本转码 → 跑测 → 全量转码 → 跑测 | 2000 帧级：`decoded frame … bytes=345600`；截图正常；19 个文件字节数与 host 一致 |
| **P2a** | 修 `astc_retier.py` 两个缺陷（§3.1-A/B）+ dry-run 自检 | dry-run 无异常；KEEP 名单与 §3.2 一致 |
| **P2b** | 全量 retier → 校验 → 同步 → 跑测 | 格式直方图 = ASTC_RGBA_8x8（保留项除外）；纹理估量 ~41 MB；**画面无"透明变不透明"** |
| **P3** | 归档数字（`[BD-MEM]` hwm、磁盘、解码帧字节数）+ 提交 | 本文 §1.4 基线一栏填上"after" |

---

## 5. ✅ Wwise 初始化已修复（2026-09-22 深夜第二轮）⇒ SoundBanks 147 MB 仍未验证

> **状态：初始化修好、warning 归零；但音频无声，`SoundBanks/` 的裁剪结论仍未定。**
> 完整根因与证据见 [`ODDMAR.md` §0.8](ODDMAR.md)，本节只记与"资源体积"相关的部分。

**一、修复结果**（`build-oddmar` = Release+LOG，60 s 跑测 `seq-wwise4`）

| 计数 | 修复前 | 修复后 |
|---|---|---|
| `Sound engine initialized successfully.` | 0 | **1** |
| `AkInitializer.cs Awake() was not executed yet` | 1512 | **0** |
| `RenderAudio` / `PostEvent` warning | 1493 / 17 | **0 / 0** |
| `Failed to initialize` / `Android initialization failure` | 1 / 1 | **0 / 0** |

三处根因：① `libAkSoundEngine.so` 的 `JNI_OnLoad` 从未被调用（JavaVM 因此没缓存）；
② `UnityPlayer.currentActivity` 用 `Landroid/app/Activity;` 查不到（描述符注册的是更具体的派生类签名）；
③ `AAssetManager_openDir` / `AAssetDir_close` / `AAsset_seek` 有实现却漏注册。细节见 `ODDMAR.md` §0.8。

**附：修复前的证据（保留作对照）**

| 证据 | 次数 | 含义 |
|---|---|---|
| `AKDEBUG: Wwise warning in AK::SoundEngine::RenderAudio(): AkInitializer.cs Awake() was not executed yet.` | **445** | 引擎**在跑**（否则这行打不出来），但 `AkInitializer.Awake()` 从未执行 |
| `AKDEBUG: … PostEvent(AkUniqueID,…): AkInitializer.cs Awake() was not executed yet.` | 24 | 游戏持续 PostEvent，**全部空转** |
| `[BD-ANY-MISS] cls=org/fmod/FMODAudioDevice str0='fmodProcess'` | **1342** | 实际音频设备后端是 **FMOD 桩**，`org.fmod.FMODAudioDevice` 未实现 |
| `[BD-AUDIO] local_fmodGetInfo(id=…) -> 2 / 4 / 1024` | 8 | 桩返回固定值，Unity 的 FMOD 通路"看起来"能用 |
| `WwiseUnity: Android initialization failure. Reason: AK_Fail` | — | 既有记录（`ODDMAR.md` §664 / §999） |

**二、对"能否裁 147 MB"的更新：结论仍是「不能裁」**

- 修复后 60 s 跑测里 **`LoadBank` 调用 0 次、`.bnk` 文件引用 0 次**，RSS 也没上升
  （`hwm` **663.4 MB**，比未修时还低 9 MB）⇒ **标题界面阶段根本不碰 SoundBank**。
- 但这**不等于死重量**：Wwise 起来之后 bank 是**运行时按需加载**的（过场 / 关卡 / UI 音效）。
  **要下结论必须进关卡**，标题界面测不出。
- `Audio/` 那 12 MB 是 `.wem`（Wwise 编码音频），同样按需读。
- 顺带：原先**逐帧**刷的 `RenderAudio` warning（1493 行 / 90 s）**已归零** —— 这本身就是一笔
  实打实的性能收益，与资源体积无关。

**三、下一步（两件事独立，别混在一起判）**

1. **进关卡**跑一轮 —— 看 `.bnk` 是否被 open、RSS 是否跳。这是裁 147 MB 的**唯一**判据。
2. **让它出声** —— Wwise 的输出后端要 OpenSL ES，而 `thunks/opensles/opensles.cpp` 默认关闭
   （关掉是为了让 libunity 自己的 FMOD 走 `org.fmod.FMODAudioDevice` → `SDL_QueueAudio`）。
   开这个 shim 会**同时改变 FMOD 的选路**，必须实测；更稳的做法是在 `dlsym_impl` 里按调用者模块分派。
   ⚠️ **「没声音」不等于「没加载 bank」** —— 两件事要分开判断。

```bash
# 验证 SoundBank 是否被读（改名后进关卡跑一轮，比对 .bnk open 次数 + RSS，再改回）
docker exec GlES_Dev bash -lc 'cd /game/Oddmar/gamedata/assets && mv SoundBanks SoundBanks.off'
docker exec GlES_Dev bash -lc 'cd /game/Oddmar/gamedata/assets && mv SoundBanks.off SoundBanks'
```

---

按体积排序，另外两块（也在 `gamedata/assets` 565 MB 里）：
2. **`bin/Data/74c32ced…` 28.3 MB，内容是 1 个 MonoBehaviour**（`types=1`）—— 单个序列化对象的体积异常，
   值得确认它是不是关卡时间轴/大二进制数据，能否换外部资源。
3. **807.8 MB 的 RSS 峰值仍未归因** —— 贴图约 98 MB、视频走流式（解码缓冲数十 MB），
   剩下的大头在 Unity 堆 / il2cpp / Wwise / 文件读取缓冲之间。**做这两项之前先量一次归因，能避免事后误判收益。**
   手段：`[BD-MEM]` 每 2 s 记录一次 + 关键节点（`libAkSoundEngine` 载入、bundle 载入、视频首帧）分点打点。

---

## 6. 验收判据（照 `ODDMAR.md` §0.1 / §⛔，四个都要看）

```bash
docker exec GlES_Dev bash -lc '
  cp /game/Oddmar/unity.toml.bak-rd9 /game/Oddmar/unity.toml
  cd /game/Oddmar &&
  BOOT_LOADER=/workspace/Bogodroid/build-oddmar-verbose/unityloader \
  SEQ_DIR=/game/Oddmar/seq-sl1 SECS=90 BD_ENABLE_LOG=1 bash /run-oddmar-seq.sh > seq-sl1/run.out 2>&1
  echo "--- 计数 ---"
  for k in ZZZSENTINEL "Unable to read header" "could not translate" CRASH SEGV NullReferenceException; do
    printf "%-26s %s\n" "$k" "$(grep -ac "$k" /game/Oddmar/log-seq.txt)"
  done
  echo "--- 视频 ---";  grep -a "decoded frame=0 " /game/Oddmar/log-seq.txt | tail -1
  echo "--- 内存 ---";  grep -aoE "hwm=[0-9.]+MB" /game/Oddmar/log-seq.txt | sort -u | tail -1
  echo "--- 画面 ---";  wc -c seq-sl1/frame.*.gl.ppm; tail -3 seq-sl1/timeline.txt'
```

| 判据 | 通过条件 |
|---|---|
| `ZZZSENTINEL` | = 0 |
| `Unable to read header` | = 0 |
| `could not translate` | = 0 |
| `[BD-ANDROID] CRASH` / `[BD-SEGV]` / `terminate` / `NullReferenceException` | 全 0 |
| `[BD-MEDIA] decoded frame=0 … bytes=` | 640×360 → **345600**（原 3110400） |
| X 根窗口截图 | **≥180 KB**（基线 290–300 KB）；`timeline.txt` 不能只有 192 B |
| `[BD-MEM] hwm` | 记录并与 807.8 MB 对比（预期 −60 ~ −100 MB） |
| 画面人工判读 | 标题界面正常、字不糊、**透明处不是实心方块**、过场能播 |

---

## 7. 复现命令（本次分析脚本在 staging，未入 git）

```bash
A=D:/Locke/gitee/LinuxArmPorts/oddmar_port_stage/_analysis
PY=C:/dev/Python310/python.exe          # UnityPy 1.25.3 + astc_encoder 已装在这里
FF=C:/dev/ffmpeg/bin/ffmpeg.exe         # 8.1，带 libx264 / aac

# 1) 资源清单（713 个容器 → 格式直方图 / 每张贴图的 VRAM 估算）
$PY $A/inventory.py  &&  $PY $A/summarize.py      # → inventory.json
# 2) 贴图分级计划（KEEP / ASTC-8x8 / downscale 候选 + alpha 与 Sprite 引用标记）
$PY $A/texplan.py                                  # → texplan.csv
# 3) 视频批转（不动源文件；输出到 video_out/<档位>/）
$PY $A/encode_videos.py "<assets>/Videos" "$A/video_out/640x360" 640 360 --crf 22 --vbr 700
# 4) ASTC spike（用运行时补丁绕开 SerializedFile 缺陷，验证工具可行）
$PY $A/retier_spike.py "<某个 bin/Data 文件>" -o "$A/spike_out/x.assets" --include-raw --block 8x8
```

视频参数基线（`ffprobe`）与三档结果见 §2.2；ASTC spike 结果见 §3.1-A。

---

## 8. 相关文档

- [`ODDMAR.md`](ODDMAR.md) —— 接手主文档（§0 现状、§⛔ 第 0 步、§8 开关、§9.2 跑测）
- [`H700_VIDEO_DECODE_SPIKE.md`](H700_VIDEO_DECODE_SPIKE.md) —— **必读**：H700 硬解 No-Go、854×480 软解实测 23–28 fps、推荐"离线 854×480@30 低参考帧"
- [`PORTING_PLAYBOOK.md`](PORTING_PLAYBOOK.md) §0/§1 —— 资源体量粗估与止损判据
- [`../tools/unity_astc/README.md`](../tools/unity_astc/README.md) —— ASTC/音频/视频工具链全参数
- [`../ANBERNIC.md`](../ANBERNIC.md) §1 —— 真机约束（640×480 / Mali-G31 / GLES 3.2）

---

## 9. 执行记录（2026-09-22 晚）

### 9.1 落地清单

| 项 | 动作 | 结果 |
|---|---|---|
| **P0 备份** | staging 的 `Videos/` `Bundles/` `bin/` → `_orig/*.orig` | 三份「文件字节数排序后 md5」与源**逐位一致** |
| **视频** | 19 个文件 → 854×480（capped-CRF23 / `-maxrate 700k` / `-refs 2 -bf 2`），另加 `RawAssets/StoryVideos/game-start.m4v` | **232.46 → 132.83 MB（−42.9%）** |
| **hook** | `bypass_video_translate` 改为从 **x23** 取 URL、逐文件映射（§2.4） | ✅ 跑测实测生效 |
| **工具** | `astc_retier.py` 修 SerializedFile 崩溃 + 补 `--alpha-mode auto`（§3.1） | dry-run + 实跑通过 |
| **贴图** | 24 个候选 → **22 个容器**实做 ASTC 8×8（`--include-raw --include-compressed --packer original --compact`、`--alpha-mode auto`）；剔除 1 个分片文件 + 1 个空候选 | ✅ 磁盘 −42.87 MB、VRAM −104.0 MB，见 §9.3 |
| **校验** | 新增 `_analysis/verify_astc_out.py`（逐容器逐纹理对照源：格式 / alpha / 尺寸 / path_id） | ✅ 22 容器全 clean |
| **Wwise** | **2026-09-22 深夜第二轮已修复**（不属本轮瘦身范围，详见 `ODDMAR.md` §0.8）：① 补调 `libAkSoundEngine` 的 `JNI_OnLoad`；② 给 `currentActivity` 补 `Landroid/app/Activity;` 别名；③ 补注册 `AAsset*` 三个符号 | ✅ `Sound engine initialized successfully.`；1512 + 1493 条 warning 归零；无声问题见 §5 |

### 9.2 跑测（`build-oddmar` + `BD_BYPASS_VIDEO_TRANSLATE=1`，`SECS=75`）

**判活四件套**：`ZZZSENTINEL`=0、`Unable to read header`=0、`could not translate`=0，日志 187 921 行；
截图 61 帧、最大 **233 787 B**（末 3 帧 84.9 / 94.1 / 138.8 KB）；loader 侧 `glReadPixels` **921 615 B × 3**
（= 640×480×3 + 15 B 头）⇒ **画面正常**。

**视频通路 —— 本次最关键的一条**

```
[BD-MEDIA] xlat: ok 'jar:file://!/assets/Videos/mobge_and_senri_splash_video.mp4'
                        -> .../Videos/mobge_and_senri_splash_video.mp4 (49712 bytes)
[BD-MEDIA] xlat: ok 'jar:file://!/assets/RawAssets/StoryVideos/game-start.m4v'
                        -> .../RawAssets/StoryVideos/game-start.m4v (5181447 bytes)
[BD-MEDIA] decoded frame=0 format=0 bytes=614880 pts=0 range=16..128
```

1. **第二条不是 splash** ⇒ 逐 URL 映射真的生效（旧 hook 会把两条都返回 splash 路径）。
   而且命中的是**转码后**的 `game-start.m4v`（5 181 447 B）⇒ 18 个关卡过场进关卡后走同一条链。
2. `bytes=614880` = 854×480×1.5；基线是 **3 110 400**（1080p）⇒ **解码/上传量 0.198×**。
3. `xlat ok=2`、`FALLBACK=0` —— 零回退。

| 指标 | 基线 | 视频-only 轮 | **最终轮（视频+贴图）** |
|---|---|---|---|
| `[BD-MEM] hwm` | **807.8 MB** | 707.3 MB | ✅ **672.7 MB** |
| 每帧 YUV | 3 110 400 B | **614 880 B** | **614 880 B** |
| `Invalid Reference` / `GL error` | — | 0 / 0 | **0 / 0** |

> ✅ **已补齐**：上面视频-only 那轮（707.3 MB）跑的是旧贴图，**不能拿来算贴图收益**。
> §9.3 末给出了**同条件对照实验**（同 loader / 同 120 s / 同路径，只差贴图）：
> 原始贴图 **704.8 MB** vs ASTC 贴图 **672.9 MB** ⇒ **贴图贡献 −31.9 MB**。
> 注意 hwm 是**峰值且含视频解码缓冲**，不是"贴图 VRAM 省了多少"——后者看离线的 §9.3。

### 9.3 贴图批次结果（24 候选 → 22 容器实做）

#### 9.3-A ⚠️ 24 个候选里有 2 个必须剔除（其中一个会静默产出坏文件）

**(1) `bin/Data/sharedassets0.assets.split0` —— 分片 serialized file，绝对不能碰。**

Oddmar 的 `sharedassets0.assets` 被 Unity 切成 **`split0..split5`**（5×1 MiB + 419 312 B）：
**split0 持完整 header，split1..5 是被 header 内偏移寻址的纯数据续片**。
UnityPy 把每片当**独立文件**打开，重存时只重写 split0 的 header 及其偏移，**split1..5 原封不动**
⇒ 每条跨片偏移全部悬空。实测 **1 MiB 进 → 4.68 MiB 出**，纯坏。

而它之所以进了候选，是因为 header 里的 metadata 完整，`texplan.csv` 照样列出
`menu`(1024²)/`loading`(512²) 两张 ETC2_RGBA8 —— 但**收益只有 ~320 KB VRAM**，
根本不值得去做"先把 6 片合并再转"这件事。

> 处理：产物留档 `_slim/_rejected/sharedassets0.assets.split0.bad`；
> `batch_astc.py` 已加 `SPLIT_RE = re.compile(r"\.split\d+$")` 防护，重跑不会再选它。
> **通用教训**：`classify` 只看"容器里有没有贴图 + 收益够不够"是不够的，
> 还要问"这个容器**是不是一个可以独立重存的整体**"。

**(2) `bin/Data/f69482386ca0c4f4aa6cecd069869958` —— dst=0，属正常跳过。**

`astc_retier.py` 在 `candidates` 为空时打印 `Nothing to do` 并直接 `return`，**不写输出文件**。
所以 `dst = 0` 的含义是"里面可转的贴图全被 skip 名单命中"，**源文件保持原样**（正确行为，不是失败）。
统计时要按"未改动"计，别当成 −100% 的收益。

#### 9.3-B 批次与结果

驱动 `_analysis/batch_astc.py`（容器级并行 4 × 容器内编码线程 6）；
UnityFS 容器加 `--compact --packer original`，裸 SerializedFile 不加（`save()` 忽略 packer）。

| 容器类 | 数量 | 纹理 | 磁盘 | 降幅 |
|---|---|---|---|---|
| `Bundles/bundle1..8`（UnityFS） | 8 | 126 | 37.07 → **25.09 MB** | −32.3% |
| `bin/Data/<md5>`（裸 SerializedFile） | 14 | 14 | 37.76 → **6.87 MB** | −81.8% |
| **合计** | **22** | **140** | **74.83 → 31.96 MB** | **−57.3%（省 42.87 MB）** |

> UnityFS 那 8 个降幅小，是因为它们多数纹理**本来就在 512 px 以下**（ETC2 0.5 B/px → ASTC 8×8 1 B/px
> 对小块并不省），且 `--compact` 后还有容器头/其它对象占用。裸 SerializedFile 是"一容器一贴图"，
> 大图直接从 ETC2/RGBA32 落到 ASTC 8×8，所以降幅大。

#### 9.3-C 全量校验（`_analysis/verify_astc_out.py`）

新写的校验器**逐容器、逐纹理**对照源：格式是否为期望的 ASTC 变体、
尺寸是否没变、`path_id` 是否都在（不丢对象）、**带 alpha 的源是否落到了 `ASTC_RGBA_*`**。

```
containers: 22   textures converted: 90   untouched: 155
texture VRAM: 140.9 MB -> 36.9 MB  (73.8% saved)
all containers clean          # mismatch=0  missing=0
```

- **纹理 VRAM 140.9 → 36.9 MB（−104.0 MB，−73.8%）** —— 这是瘦身对显存/内存的真正贡献（离线口径）。
- 155 张"未动"= skip 名单（字体/水印/debug UI/小收益）+ KEEP 项，属预期。
- ⚠️ alpha 判据是**必须**查的：源带 alpha 却选了 `ASTC_RGB_*` 时，离线完全看不出来——
  GPU 采样非 alpha 内部格式会把 alpha 读成恒 1.0，只有跑起来才现形为"抠图变实心块"（§3.1）。

#### 9.3-D 运行时对照实验（同条件，只差贴图）

同一 loader（`build-oddmar`）、同 `SECS=120`、同 `CAP_MS=1000`、同路径，只替换贴图：

| 指标 | 原始贴图 | ASTC 贴图 | 差 |
|---|---|---|---|
| `[BD-MEM] hwm` | 704.8 MB | **672.9 MB** | **−31.9 MB** |
| 日志行数 | 312 699 | 308 221 | 相当 |
| 有画面帧 | 56 / 65 | 55 / 65 | 相当 |
| `GL error` | 0 | **0** | — |
| `bundle1` 读取尺寸 | 13 498 260 | **9 259 443** | ASTC 生效 ✅ |

**画面**：两轮都稳定进入**标题界面**（`ODDMAR` logo + 角色画 + 篝火场景，截图 ~277–296 KB），
无花屏、无纯色块、无抠图变实心。

> ⚠️ **别用逐像素 diff 比较这两轮**。标题界面有篝火动画、萤火虫粒子、光晕，两轮的**游戏内动画相位不同**，
> 所以跨轮同帧号 diff（mean 6.3–12.6/255）与**同一轮内相邻帧**的 diff（mean 8.3/255）是**同一量级** ——
> 差值全部来自动画，与贴图无关。要证明贴图正确，靠的是上面三样：
> 离线全量校验、`GL error = 0`、以及**肉眼确认画面结构一致**。

**最终交付态复跑**（`SECS=90`，ASTC 贴图已就位）：日志 225 772 行、41/51 帧有画面、
四件套 `0/0/0/0`、`GL error = 0`、**hwm 672.7 MB**。

#### 9.3-E 磁盘落盘检查

容器内 `gamedata/assets`：`Bundles/` 26 MB（原 38 MB）、`bin/Data/` 97 MB（原 128 MB，仅替换 14 个文件）、
`Videos/` 132 MB（原 232 MB）。**22 个文件字节数逐个核对与 staging 一致**（`docker cp` 后 `stat -c %s`）。

### 9.4 尚未闭环的三项

1. **18 个关卡过场**：标题界面只请求 splash + game-start（`extractor new` = 2），
   要**进关卡**才能看到 `W*L*.m4v` 的 `xlat` 行。⚠️ 这也意味着**当前无法端到端验收 18 个过场**。
2. **内嵌 `unlockAllLevels`**（§2.5，12.1 MB）本轮未做。
3. ⚠️ **`bin/Data/<md5>` 那 14 个贴图容器在标题界面阶段从未被打开** —— 日志里
   `open assets/Bundles/bundle1..8` 有 8 条，**`open assets/bin/Data/*` 一条都没有**。
   也就是说：**那次 −104 MB 的 VRAM 收益里，裸 SerializedFile 那部分（−31 MB）在本次跑测中还没被实际消费**，
   它们要等进关卡才会上场。hwm 的 −31.9 MB 基本来自 Bundles 那 8 个容器。
   ⇒ **进关卡后的第二次对照才是有意义的端到端内存验收**。

### 9.5 容器测试的固有局限（别误读性能）

`GlES_Dev` 是 **aarch64 镜像跑在 x86_64 宿主（Ryzen 9 7950X）+ QEMU 用户态模拟**（`uname -m` = aarch64，
`/proc/cpuinfo` = AMD Ryzen 9 7950X）。所以：

- **能验**：功能正确性（不崩、画面、格式、字节数、映射）。
- **不能验**：**帧率 / 解码耗时 / 真机内存上限**。QEMU 与 A53 的性能关系没有可比性。
  `H700_VIDEO_DECODE_SPIKE.md` 里 854×480 的 23–28 fps 是**真机**数据，那才是选档依据。

### 9.6 本轮新踩的判读坑（跑测前先读）

**(1) ⚠️ `run-oddmar-seq.sh` 会覆盖 `log-seq.txt` —— 跑下一轮前先备份。**

本轮有一次把成功的视频轮日志直接覆盖掉了，只剩截图。习惯做法：

```bash
cp /game/Oddmar/log-seq.txt /game/Oddmar/log-seq.<标签>.bak   # 跑测前
```

**(2) ⚠️ `timeline.txt` 的字节数在「第 4 列」，不是第 3 列。**

行格式是 `%04d %6d ms %8d B` ⇒ `$1=序号 $2=毫秒 $3="ms" $4=字节数 $5="B"`。
用 `awk '$3 != 192'` 判"有画面"会**恒真**（`"ms" != 192`），得出"163/163 帧都有画面"这种反向结论。
正确写法：

```bash
awk '$4 > 192' seq-xxx/timeline.txt | wc -l     # 有画面的帧数
```

**(3) ⚠️ 启动期偶发阻塞：日志只有几百行 = 没起来，不是资源问题。**

本轮遇到过一轮 `SECS=130` 全程 **0 帧画面**、`frame.*.gl.ppm` 一个都没生成。判读：

```
日志 575 行（正常轮 17.7–31 万行）
末行序列： [BD-DEVICE] SDL display mode: 640x480 @ 0Hz
           [BD-EGL_SDL] before window display=0 current_mode=640x480@0 ...
           [BD-PREFS] saved ...
           Caught signal, fast-exiting via _exit      ← SIGINT 正常退出，不是崩
```

即**走到 EGL 窗口创建就停住**，之后没有任何渲染/资源加载日志。
**这不是资源/贴图问题**：紧接着用**完全相同的资源**复跑一次就正常了（142 545 行、38/47 帧有画面）。
⇒ **判"资源有没有搞坏"必须至少跑两轮**，单轮 0 画面先怀疑启动竞态。
**看日志行数是最快的分诊手段**：几百行 = 启动期死；十几万行以上 = 真的跑起来了。

**(4) `[BD-PREFS]` 的 `splash_screen_displayed` 会随轮次累积**（`conf/shared_prefs/com.mobge.Oddmar.v2.playerprefs.json`）。
本轮它并不影响上述阻塞（有/没有该键都跑成功过），但**排查启动差异时先看它**，
`.orig` 备份在同一目录，对比即可。
