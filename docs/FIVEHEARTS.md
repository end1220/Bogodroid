# FiveHearts — 运行、复现与视频桥

> 本文档记录 FiveHearts（`com.fivehearts.offline`，Unity 2022.3.62f2，arm64）在这套
> loader 上的**两种运行方式**（掌机 / Docker 容器）、**VideoPlayer 视频桥的架构**，
> 以及 2026-09-16 定位到的「视频黑屏但解码正常」根因与修法。
>
> 掌机运行方式最早是会话里手敲出来的，这里固化，不要再凭记忆拼命令。
>
> 视频黑屏、面板偏移已修复；`textureMaxDim` 误缩 RenderTexture 导致的裁切目前用
> `textureMaxDim = 0` **绕过**（正经修法仍待做，见 §4.6 B）。关键判据与通用结论见
> [`CASE_STUDIES.md`](CASE_STUDIES.md)。

## 当前结论（先看）

**已可用**：游戏可启动；视频可见且颜色正常；`yuv_gpu` 已把 I420→RGBA 从 CPU
移到 GLES；异步 decode worker 已上机，720p 高复杂度 intro 从同步基线 12–15fps
升到 20.6–21.0fps，后续 clip 可到 29–31fps。上机默认改为**中间态日志**
（`BD_ENABLE_LOG=ON`，TRACE/VERBOSE 关）：LOG 层保留 `[BD-MEM]`、`publish`、
`swap`、codec 周期摘要、worker 启停、`[STUB-MISS]`；逐帧 blit / upload /
guest step / luma / handoff 等已降到 `BD_DEBUG`（需 TRACE）。精简到此为止，
不再为再压行数继续砍。

**主动 seek 已验收**（2026-09-18）：`SimpleVideoPlayer` SeekProbe 掌机实测 —
D-Pad 跳转、`decode worker` flush/restart、稳态 `publish≈19–20 fps`、
`drop=0`、`exited (0)`；事件写入 `conf/seekprobe.log`（IL2CPP Release 的
`Debug.Log` 进不了 loader stderr）。

**仍待解决（§4.6 B）**：

1. **`textureMaxDim` 正确识别 RenderTarget**：当前只能设为 `0` 绕过裁切，尚未兼得
   纹理省内存与 RT 尺寸正确。

**暂缓**：H700 硬解接口不通；全局“稍微偏白”目前无法复现。

---

## 0. 两种运行方式各自适合什么

| 方式 | 命令入口 | 适合 |
|---|---|---|
| 掌机 | `scripts/fivehearts_launch.sh`（Dropbeak 推上去执行） | 最终验收：真实 Mali GL、真实音频、真实输入 |
| Docker 容器 | `scripts/fivehearts/run.sh` / `frames.sh` | 迭代排障：aarch64 + qemu，**可逐秒抓帧**，不占用掌机、不会把掌机跑死 |

两者跑的是**同一个二进制**（`build-anbernic-rel/unityloader`，aarch64 + glibc 2.31）。
容器只是用 Mesa llvmpipe + Xvfb 顶替 Mali/EGL，所以 GL 行为会有差异（见 §5）。

---

## 1. 掌机布局与启动

端口目录 `/mnt/mmc/Roms/PORTS/FiveHearts/`（主机侧 staging 同名副本：
`D:\Locke\gitee\LinuxArmPorts\fivehearts_port_stage\FiveHearts\`）：

```
FiveHearts/
├── unityloader          ← build-anbernic-rel/unityloader
├── unity.toml           掌机配置（game_files="./gamedata/"）
├── gamedata/            lib/arm64-v8a/{libil2cpp,libmain,libunity}.so + assets/
├── conf/                存档 / SharedPreferences（游戏写）
├── cache/               UnityShaderCache（Unity 写，见 §5）
└── log.txt              运行时 stderr
```

**必须走前端交接**：`dmenu.bin` 持有 framebuffer，直接 `dropbeak exec` 启动会跟它
抢显示并输掉。交接契约是「把命令行写进 `/tmp/.next`，然后给 `dmenu.bin` 发
`SIGUSR1`」，前端自己拆 UI 再 exec 那一行。`scripts/fivehearts_launch.sh` 就是干这个的。

标准循环（**验证前后都要 kill**，否则 unityloader 占满 CPU，dropbeak 会连不上）：

```bash
DROP=dist/dropbeak-cli.exe     # 在 D:/Locke/gitee/dropbeak

# 0) 先清干净
$DROP exec "pkill -9 unityloader; sleep 1; pgrep -l unityloader"

# 1) 推二进制（--verify 依赖 --chunk；覆盖现有文件还要 --force）
$DROP push unityloader-video-debug /mnt/mmc/Roms/PORTS/FiveHearts/unityloader \
  --force --chunk --chunk-size 16m --verify --progress

# 2) 启动
$DROP exec "sh /mnt/mmc/Roms/PORTS/FiveHearts/relaunch.sh"

# 3) 等游戏跑到 intro，再收日志
$DROP exec "grep -a 'BD-VIDEO' /mnt/mmc/Roms/PORTS/FiveHearts/log.txt | tail -30"

# 4) 收工必须 kill，不然掌机会卡死
$DROP exec "pkill -9 unityloader"
```

推之前先在主机侧确认二进制没被截断：

```bash
docker exec GlES_Dev md5sum /workspace/Bogodroid/build-anbernic-rel/unityloader
docker cp GlES_Dev:/workspace/Bogodroid/build-anbernic-rel/unityloader ./unityloader-video-debug
ls -la unityloader-video-debug       # 期望 ~几十 MB（debug 版，未 strip）
```

---

## 2. Docker 容器运行（不占掌机）

镜像 `bogo-arm64-test:20.04`（`Dockerfile.test`）：ubuntu 20.04 的 aarch64 rootfs，
qemu binfmt 跑起来，外加 Xvfb + Mesa llvmpipe 顶替 Mali，以及 loader 依赖的全部
运行库 —— **包括 FFmpeg 4.2 的 `libavformat58/libavcodec58/libavutil56/
libswresample3/libswscale5`**（`thunks/ndk/media.cpp` 的 MediaCodec 后端要用；
unity6 分支那版镜像没装，FiveHearts 跑不起来是因为 id.so 解析不到 `libavcodec.so.58`）。

```bash
# 建一次（改 Dockerfile.test 后重建）
docker build --platform linux/arm64 -t bogo-arm64-test:20.04 -f Dockerfile.test .
```

游戏根目录直接挂主机上的端口目录副本，二进制和容器版 toml 另挂到 `/boot`：

```bash
GAME="D:/Locke/gitee/LinuxArmPorts/fivehearts_port_stage/FiveHearts"
REPO="D:/Locke/gitee/Bogodroid-anbernic"
cp unityloader-video-debug "$REPO/tmp/docker-boot/unityloader"

# 单张截图
docker run --rm --platform linux/arm64 \
  -v "$GAME:/game" -v "$REPO/tmp/docker-boot:/boot" \
  -v "$REPO/scripts/fivehearts:/work/scripts/fivehearts" \
  -e SECS=40 bogo-arm64-test:20.04 bash /work/scripts/fivehearts/run.sh

# 逐秒抓帧时间线（黑 → splash → intro → lobby）
docker run --rm --platform linux/arm64 \
  -v "$GAME:/game" -v "$REPO/tmp/docker-boot:/boot" \
  -v "$REPO/scripts/fivehearts:/work/scripts/fivehearts" \
  -e SECS=40 bogo-arm64-test:20.04 bash /work/scripts/fivehearts/frames.sh
```

抓到的帧在 `$GAME/frames/fNN.png`，日志在 `$GAME/log.txt`。

### 2.1 看帧：三个工具

| 工具 | 干什么 |
|---|---|
| `scripts/fivehearts/analyze.sh` | 每帧一行：mean / sd / min / max / 中心 50% 区域均值。`sd≈0` 是纯色（splash/黑屏），`sd` 几十 = 真实画面 |
| `scripts/fivehearts/look.sh` | 把帧降采样成 ASCII 亮度图（`0`=黑 `f`=白）。分辨「全黑」「黑但有 UI 条」「中间有画面」比肉眼看 PNG 快 |
| `scripts/ppm2png.py` | 老的 PPM 帧转 PNG（`BD_VIDEO_DUMP` 路径用的） |

```bash
docker run --rm --platform linux/arm64 \
  -v "$GAME/frames:/f" -v "$REPO/scripts/fivehearts:/work/scripts/fivehearts" \
  bogo-arm64-test:20.04 bash /work/scripts/fivehearts/analyze.sh /f
```

**只截一张图不够**：引擎前几帧本来是全黑，splash 也是纯色，必须看时间线。

---

## 3. 视频桥架构（为什么需要它）

Unity 的 `VideoPlayer` 在 Android 上走 `AndroidVideoMedia`：`AMediaExtractor` 解封装
→ `AMediaCodec` 解码 → 帧进 `SurfaceTexture` 的 `BufferQueue` → GL 通过
`GL_TEXTURE_EXTERNAL_OES` 采样。

掌机上没有 gralloc/BufferQueue/gralloc HWC，于是这条链的后半段全部由 loader 接管：

```
FFmpeg (thunks/ndk/media.cpp)
   └─ AMediaCodec 解出 I420 帧
        └─ javastubs/bd_video.cpp   submit_i420(): 保存 I420，发布
             └─ bd_video::pump()    渲染线程（eglSwapBuffers）调用
                  └─ 通知 guest 的 OnFrameAvailableListener
                       └─ guest 调 SurfaceTexture.updateTexImage()
                            └─ upload hook = thunks/khronos/gles2.cpp bd_video_present()
                                 └─ 上传 Y/U/V，GPU 色转到 GL_TEXTURE_2D（backing）
                                      └─ 视频四边形绘制时采样该纹理
```

关键约定（三处都在代码注释里重复过，别只改一处）：

* **`bd_video::pump()` 必须在渲染线程、GL context current 时调用**。guest 的
  `updateTexImage()` 是同步的，上传跟着它走；在 media 线程调就是没有 context 的上传。
* **只接 texture id 是不行的**：Unity 传给 `SurfaceTexture` 的 handle（22/176）
  和它绑定 `GL_TEXTURE_EXTERNAL_OES` 的 GL name（179）不是同一个数，所以
  `bd_glBindTexture` 里凡是 external bind 一律重定向到 backing 纹理。
* **采样器必须改写**：`glShaderSource` 把 `samplerExternalOES` 改写成 `sampler2D`，
  否则 shader 读的是永远没有数据的 external 纹理。
* **分辨率 / 码率不在 loader 里压**：`submit_i420()` 按源帧尺寸做 I420→RGBA 上传
  （不再 `1280x720→640x360`）。画质与 CPU 预算由 **Unity 构建 APK 前的离线转码**
  决定。本标题最终保留观感较好的 **1280×720@30、H.264 High、B 帧=2、
  约 900 kbps、BT.709 TV range**；异步 worker + 无日志 Release 已使它达到可接受
  流畅度。854×480 / 960×540 仅作为以后需要进一步降 CPU 或体积时的备选。源帧比
  drawable 大约 25% 时会打一条 `source … larger than drawable` 警告。

---

## 4. 已完成工作、性能与待办

### 4.1 已解决问题摘要

以下问题已解决，只保留维护所需结论；详细取证见
[`CASE_STUDIES.md`](CASE_STUDIES.md)：

- **视频黑屏**：Unity GL state cache 在 draw 前覆盖 external→2D 重定向。现于实际
  draw 前重绑 backing texture，并用 shader-cache 版本戳淘汰旧程序。
- **面板偏移**：启动前对齐 fb0 与 640×480 drawable。
- **GPU 色转首版近乎全白**：FBO backing 覆盖 Y 采样单元形成反馈；draw 前重绑
  Y/U/V 后修复。
- **立即退出 134**：曾由 UTF-16 `unity.toml` 引起；配置必须是 UTF-8。
- **主动 seek**（2026-09-18）：SeekProbe 掌机验收通过，见 §4.6 A。

`textureMaxDim` 裁切的**症状已绕过但机制未修复**，归入 §4.6 B，不列为已解决。

### 4.2 I420 三平面 GPU 色转（2026-09-17）

`[video] path = "yuv_gpu"`（默认）不再在 `submit_i420()` 热路径逐像素转 RGBA：

1. media 线程只复制紧凑 I420，保留 FFmpeg 的色彩矩阵/范围；
2. render 线程上传 Y/U/V 三张 R8 纹理；
3. 私有 GLES3 shader 转成 RGBA，画入原有 backing texture；
4. Unity 的 external→2D 改写、VideoPlayer blit 和 RawImage 契约保持不变。

掌机 854×480 实测：I420 handoff 约 **0.56–0.61ms/帧**，三平面上传+GPU blit
稳定后约 **1.9ms/帧**，`publish` **23–28fps**；旧 `rgba_cpu` 路径约
**11ms/帧**（早期 1280×720 约 21ms）。GPU shader/FBO 初始化失败会自动退回
`rgba_cpu`。

色彩默认 `auto`：优先使用 FFmpeg 解码帧元数据，元数据缺失时 HD 用 BT.709
limited、SD 用 BT.601 limited；可用 `[video] color_matrix`（`auto/bt601/bt709`）
和 `color_range`（`auto/limited/full`）诊断覆盖。

H700 VPU 接入探测与 No-Go 依据见
[`H700_VIDEO_DECODE_SPIKE.md`](H700_VIDEO_DECODE_SPIKE.md)。

### 4.3 当前状态与后续优化边界（2026-09-17）

当前掌机验证基线：

- 异步 worker 诊断版 SHA-256：`be537105…d87097`；
- 当前无日志 Release SHA-256：`5a733b35…7fbcf37`；
- `[video] path = "yuv_gpu"`、色彩矩阵/范围均为 `auto`；
- `[gpu] textureMaxDim = 0`；失败的 sRGB 实验代码未保留；
- 当前视频为 1280×720@30、High、B 帧=2、约 900 kbps、BT.709 TV range；
- 不同离线编码的帧率对照见 §4.5；待办清单见 §4.6。

这里的“GPU 优化”不是 H.264 硬解。H.264 仍由 FFmpeg 在 CPU 上解成 I420；
GPU 只接手原先由 CPU 完成的 I420→RGBA 色转。现阶段最大的剩余成本是软解本身，
不是 I420 搬运或 GLES 色转。

软解条件下的后续边界（不替代 §4.6）：

1. 无日志 Release 已复测通过；日常部署继续使用该构建。
2. `BD_MEDIA_THREADS` 保持 FFmpeg 默认值；当前观感已达标，不再为几帧收益增加
   与 Unity/音频争核的风险。
3. 若以后还要降 CPU 或体积，优先试 960×540，再考虑 854×480（见 §4.5）。
4. 双 PBO/减少 GL 状态保存只能优化约 1.9–3 ms 的上传色转，优先级低。

H700 硬解当前仍是 No-Go：芯片有 Cedar 引擎，但系统没有 aarch64 CedarX，
也没有可供 FFmpeg 接入的 V4L2 request/M2M H.264 decoder。除非补齐这些接口，
否则不能把 32 位厂商 CedarX 库直接装进 64 位 unityloader。详见
[`H700_VIDEO_DECODE_SPIKE.md`](H700_VIDEO_DECODE_SPIKE.md)。

### 4.4 “稍微偏白”调查暂结（2026-09-17）

Mali 虽回报 sRGB capable，但试验性启用后实屏更白，不能信任该 capability；相关
代码和配置未保留。`/dev/fb0` 的 swap 300/600/900 抓帧及后续手工启动均正常，问题
暂时无法复现。若重现，以 `/dev/fb0` 和 Android 同帧数值对比；Mali 下
`glReadPixels` 曾返回全黑，不能单独作为颜色证据。

### 4.5 离线编码与异步 worker 对照（掌机，`yuv_gpu`，2026-09-17）

前三行是异步 worker 前、同一 loader（SHA `93d72132…`）只换 APK 内视频编码的
对照；最后一行是当前异步 worker 诊断版。指标来自运行日志：

- `publish:` — 客人真正看到的发布帧率（每 30 帧一条，稳态区间）
- `decode=` — FFmpeg `send+drain` 墙钟时间
- handoff / GPU — I420 交接与三平面上传+色转

| 构建 / 意图 | 分辨率（日志） | intro.bundle | lobby.bundle | `publish` 稳态 | `decode` | handoff | 上传+GPU |
|---|---|---:|---:|---|---|---|---|
| 面板友好（约 854 宽） | **854×480** | — | — | **23–28 fps** | **~10.8 ms** | **~0.56 ms** | **~1.9 ms** |
| 720p 低解码开销 | **1280×720** | 7,223,280 | 13,020,048 | **17–20 fps**（常见 18–20） | **~14–16 ms** | **~1.4–1.5 ms** | **~2.9 ms** |
| 720p 更高画质 | **1280×720** | 8,000,480 | 14,684,112 | **12–15 fps**（常见 13–14） | **~20–22 ms** | **~1.45 ms** | **~2.95 ms** |
| 720p 高画质 + 异步 worker | **1280×720** | 8,000,480 | 14,684,112 | intro **20.6–21.0 fps**；后续 clip **29–31 fps** | **~19.8–24.4 ms**（worker） | **~1.6–1.8 ms** | **~2.9 ms** |

补充：

- 三次 `submits/publish` 均为 **1.0**：发布侧几乎不白烧解码帧；帧率差主要来自软解变慢，而不是丢帧门控。
- 同为 720p 时，handoff / GPU 几乎不变；高画质相对低开销大约多付 **5–7 ms/帧** 在 H.264 软解上，`publish` 掉约 **30%**。
- 854×480 相对 720p：像素量约少一半，handoff / GPU / 软解一起变轻，是目前最接近流畅的一档。
- 旧对照（同机、改路径前）：`rgba_cpu` 在 854 量级色转约 **11 ms/帧**，早期全尺寸 720p CPU 色转约 **21 ms/帧**；现已由 GPU 色转取代热路径。
- APK 指纹（便于复测）：低开销包 `C1AA5E37…E01479`（16:17）；高画质包 `1952B63D…31A73D2`（16:49）。视频在 AssetBundle 内，经 `AMediaDataSource` 读入；`conf/FiveHearts/video/INTRO.mp4` 若存在只是旧遗留，不参与本次 intro 播放。

最终选型：保留 **1280×720@30、High、B 帧=2、约 900 kbps、BT.709 TV range**。
中间态日志 / 无日志 Release 的实际游玩主观确认比同步软解流畅；画质与流畅度均满意，
不再为降码率或取消 B 帧牺牲观感。

### 4.6 实现状态与剩余问题（唯一权威清单，2026-09-18）

本节同时保留已完成实现和剩余待办：A（含主动 seek）已完成掌机验收；B 尚未实现。

#### A. 异步 decode worker（软解路径，已完成）

**原问题**：FFmpeg `avcodec_send_packet` / `avcodec_receive_frame` 原先跟 Unity 的
`AMediaCodec` 调用同步执行（常落在视频更新 / `UpdateTexture` 路径）。720p 高画质档
`decode≈20–22 ms/帧`，会直接拉长该路径墙钟时间；`BD_MEDIA_THREADS` 只能让 FFmpeg
内部并行，**调用线程仍要等这一帧解完**。

**目标**：独立 worker 线程持续解码；MediaCodec 桩侧只做 packet 入队与取已解帧。
Unity / 渲染相关路径不再同步支付软解时间。

**当前实现**（`thunks/ndk/media.cpp`，2026-09-17）：

1. 每个视频 codec 一个 decode worker；音频保留同步路径，避免 Unity 大量短命音频
   codec 反复建线程。
2. `queueInputBuffer` 把 packet 复制到带 FFmpeg padding 的私有输入队列后返回；
   worker 取走副本后立即归还 guest input slot。
3. worker 独占 `AVCodecContext`，执行 `avcodec_send_packet` /
   `avcodec_receive_frame`；输出写入现有 `pending`，达到
   `MAX_QUEUED_OUTPUTS` 时阻塞 worker，不丢 guest 尚未消费的帧。
4. `dequeueOutputBuffer` / `releaseOutputBuffer` 只消费已就绪帧，继续走现有
   `submit_i420` → `yuv_gpu` 路径。
5. `stop/delete` 会唤醒并 join worker；`flush`（seek）会 join、清输入/输出和
   `avcodec_flush_buffers`，然后按 started 状态重启 worker；EOS 保持队列顺序。

**Docker 功能回归**（aarch64 qemu + llvmpipe，40 s）：

- worker 有独立 tid，窗口探测 codec 与 Surface codec 均能 start/stop/join；
- `yuv_gpu` 收到 854×480 I420，handoff 约 0.05–0.11 ms，时间线进入真实画面；
- 无 `send failed` / crash / pure virtual，超时退出码 124 符合测试脚本预期；
- llvmpipe 首帧 GPU 上传约 1.1 s，容器结果不用于判断掌机帧率。

**掌机验证 — 播放性能**（loader SHA-256 `be537105…d87097`，两轮共约 190 s）：

- 1280×720 高复杂度 intro 的 `decode` 约 19.8–24.4 ms/frame，`publish`
  稳定 20.6–21.0fps；同步基线为 12–15fps；
- intro 约 60 s 处 `frame sink cleared`，旧 worker 正常 stop，新 worker 启动；
  后续 clip 稳态 29.3–30.9fps；
- `pending` 稳定 4–6、峰值 6，输入队列 3–4，`submits/publish=1.0`，
  未出现无界堆积或白解帧；
- `guest video step` 约 0.1–0.5 ms，I420 handoff 约 1.6–1.8 ms，
  GPU 上传约 2.9 ms；
- 无 `send failed`、crash、pure virtual；RSS 约 386–428 MB；
- 无日志 Release（SHA `5a733b35…7fbcf37`）实际游玩确认流畅度满意、明确高于
  21fps；该版本不含性能日志，因此不虚构精确帧率。

**掌机验证 — 主动 seek**（2026-09-18，`SimpleVideoPlayer` SeekProbe）：

- 工程：`E:\BaiduNetdiskDownload\Games\SimpleVideoPlayer`（`com.bogodroid.seekprobe`），
  端口 `/mnt/mmc/Roms/PORTS/SimpleVideoPlayer/`；循环播 `Resources/INTRO`，
  D-Pad L/R ±5s、U/D ±30s、A 暂停、B 回开头（按键与 Jump2022 Legacy 对齐）。
- 中间态 loader（`BD_ENABLE_LOG=ON`，TRACE/VERBOSE 关）：
  - 首轮节流 blit 后（SHA `32c06f14…`）：~37 s / ≈560 行 / ≈44 KB；
  - 再收热路径后（SHA `3d62e8d9…`，2026-09-18 自跑 ~47 s）：≈309 行 /
    ≈25 KB（约 **6.6 行/s**）；`publish≈19.4–19.6`、`drop=0`。
  - LOG 保留：`publish`（每 ~120 帧）、`swap` / codec dump（~5 s）、
    `[BD-MEM]`、worker 启停、启动缺桩；`get/releaseOutputBuffer` 仅前 3 次。
  - 已降到 TRACE：`guest video step`、upload/luma/handoff、`video draw`、
    逐帧 blit 探测等。**日志精简止于此。**
- `conf/seekprobe.log` 记录 prepare / pause / 5 次 seek：`seekCompleted`
  latency ≈107–977 ms；后续 seek 的 `was` 时间推进，说明跳转生效
  （`seekCompleted` 回调里读到的 `VideoPlayer.time` 有时仍是旧值，属 Unity 时序，
  以后续播放进度为准）。
- 稳态 `publish≈19.3–19.6 fps`，seek 窗口短暂掉到 ~6–14 fps（worker
  stop/restart，预期行为，不单独优化）；`drop=0`；`rss≈217–219 MB`。

**验收（掌机）**：

| 指标 | 结果 |
|---|---|
| 同 APK（尤其 720p 高画质）`publish` | 通过：12–15fps → 20.6–21.0fps |
| Unity 视频更新路径上的同步 decode | 通过：FFmpeg send/drain 已移到独立 worker |
| `submits/publish`、clip 切换、稳定性 | 通过：1.0；worker 正常 stop/restart；无崩溃 |
| 无日志 / 中间态主观流畅度 | 通过：满意，明确高于 21fps |
| 主动 seek | 通过：SeekProbe 掌机 5 次跳转 + worker flush/restart |

#### B. `textureMaxDim` 识别 RenderTarget（正经修法）

**现状**：`textureMaxDim = 0` 绕过误缩视频 RT 的裁切；开 `>0` 仍会打到
`glTexStorage2D` 创建的 RenderTexture（见 §4.1 / CASE_STUDIES 案例三）。
SeekProbe / FiveHearts 上机配置均保持 `textureMaxDim = 0`。

**目标**：内容上传可继续 cap 省内存；被 `glFramebufferTexture2D` 挂成颜色附件的
纹理（或明确的 RT 分配）不缩，viewport 与 attachment 尺寸保持一致。

**验收**：同一标题可设合理 `textureMaxDimRGBA8>0`，视频 / 后处理 RT 日志中
`attachment size == viewport`，画面不裁切；rss 相对全关 cap 有可测下降。

#### C. 次级优化（待测，非阻塞）

- `BD_MEDIA_THREADS` 保持默认，不再主动扫描。
- 双 PBO / 减少 GL 状态保存；仅覆盖约 1.9–3ms，优先级低。

#### D. 明确不做 / 暂缓

- H700 Cedar / V4L2 硬解：No-Go，见 spike 文档。
- 仅凭 SDL `FRAMEBUFFER_SRGB_CAPABLE` 向 Unity 谎称 sRGB：已证会更白，实验代码不保留。
- 「稍微偏白」全局调查：已暂结（§4.4）。
- seek 窗口内的短暂掉帧：worker flush/restart 固有成本，不做专项优化。
---

## 5. 容器 ≠ 掌机（判读差异用）

- 容器是 qemu + llvmpipe，掌机是 Mali；容器只适合功能回归和相对对照，绝对帧率、
  音频与实屏颜色必须上机验收。
- shader cache 已由版本戳管理；只有怀疑旧 loader 缓存时才手动删
  `cache/UnityShaderCache`。
- 上机发布使用 Release 且关闭 LOG/TRACE/VERBOSE；排障版日志会影响性能。

---

## 6. 调试开关速查

| 开关 | 位置 | 作用 |
|---|---|---|
| `BD_VIDEO_TRACE_UI=2` | `thunks/khronos/gles2.cpp` | 首个视频帧后 300 次 draw 普查（program/fbo/viewport/每个 sampler 的纹理号+尺寸），并 dump 采样视频纹理的 draw |
| `BD_VIDEO_TRACE_UI=3` | 同上 | 额外 dump 各 unit 上纹理的尺寸 |
| `BD_NO_FB_MODE=1` | `thunks/egl_sdl/egl_sdl.cpp` | 关掉 `fb0` 模式自愈（诊断「偏到一角」时用来做对照） |
| `BD_VIDEO_DEBUG_GRADIENT=1` | `javastubs/bd_video.cpp` | 上传内容换成渐变，验证「四边形有没有采样我们的纹理」 |
| `BD_VIDEO_DUMP_FRAME=<前缀>` / `BD_VIDEO_DUMP_AT=<帧号>` | `javastubs/bd_video.cpp` | 把 `submit_i420` 的 RGBA 落成 PPM |
| `BD_DUMP_FRAME` / `BD_DUMP_FRAME_AT` | `thunks/egl_sdl/egl_sdl.cpp` | 从 `glReadPixels` 和 `/dev/fb0` 抓帧（掌机用，容器没意义） |
| `BD_JNI_TRACE=1` | libjnivm | 全量 JNI trace（几万行） |

`[BD-VIDEO]` 日志本身是有界的（前若干次 + 每 N 次），热路径不会刷屏。

---

## 7. 相关文件

| 文件 | 作用 |
|---|---|
| `Dockerfile.test` | 容器运行镜像（aarch64 + qemu + Xvfb + llvmpipe + FFmpeg 运行库） |
| `scripts/fivehearts/run.sh` | 容器跑一局 + 单张截图 |
| `scripts/fivehearts/frames.sh` | 容器逐秒抓帧时间线 |
| `scripts/fivehearts/analyze.sh` | 帧亮度统计表 |
| `scripts/fivehearts/look.sh` | 帧 ASCII 亮度图 |
| `scripts/fivehearts_launch.sh` | 掌机启动（Dropbeak + dmenu.bin 交接） |
| `configs/fivehearts.toml` | 容器版配置（与掌机 `unity.toml` 只差 `[device]`） |
| `javastubs/bd_video.{h,cpp}` | 视频桥：I420→RGBA、发布帧、SurfaceTexture 注册 |
| `thunks/ndk/media.cpp` | `AMediaExtractor`/`AMediaCodec` 的 FFmpeg 实现 |
| `thunks/khronos/gles2.cpp` | external bind 重定向、shader 改写、绘制点重绑 |
