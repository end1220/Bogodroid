# FiveHearts — 运行、复现与视频桥

> 本文档记录 FiveHearts（`com.fivehearts.offline`，Unity 2022.3.62f2，arm64）在这套
> loader 上的**两种运行方式**（掌机 / Docker 容器）、**VideoPlayer 视频桥的架构**，
> 以及 2026-09-16 定位到的「视频黑屏但解码正常」根因与修法。
>
> 掌机运行方式最早是会话里手敲出来的，这里固化，不要再凭记忆拼命令。
>
> 视频黑屏、面板偏移和 RenderTexture 被 `textureMaxDim` 误缩导致的裁切均已修复；
> 关键判据与通用结论已归档到 [`CASE_STUDIES.md`](CASE_STUDIES.md)。

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

# 1) 推二进制（--verify 必加：push 会静默截断）
$DROP push unityloader-video-debug /mnt/mmc/Roms/PORTS/FiveHearts/unityloader --verify

# 2) 启动
$DROP exec "sh /mnt/mmc/Roms/PORTS/FiveHearts/launch.sh"   # fivehearts_launch.sh 的部署名

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
        └─ javastubs/bd_video.cpp   submit_i420(): I420 → RGBA，翻转，发布
             └─ bd_video::pump()    渲染线程（eglSwapBuffers）调用
                  └─ 通知 guest 的 OnFrameAvailableListener
                       └─ guest 调 SurfaceTexture.updateTexImage()
                            └─ upload hook = thunks/khronos/gles2.cpp bd_video_present()
                                 └─ 上传到 loader 自己的 GL_TEXTURE_2D（backing）
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
  决定。推荐掌机目标（cover / 铺满 640×480）：约 **854×480@30、~700–900 kbps、
  H.264 Baseline、B 帧=0、参考帧≤2**；暂时保留 720p 也可，但看日志
  `convert: … ms/frame` / `publish: … fps` / 音频是否卡。源帧比 drawable 大约 25%
  时会打一条 `source … larger than drawable` 警告。

---

## 4. 根因：「视频黑屏、解码正常」= Unity 状态缓存把重定向顶掉了

症状：日志里一切健康（codec 起、帧解出、`updateTexImage` 每帧调用、上传计数在涨），
屏幕上视频区域纯黑，UI 正常。

容器里抓帧 + 打点定位到的是**绘制那一刻的纹理绑定**（`thunks/khronos/gles2.cpp`）：

```
[BD-VIDEO] uploaded frame #1 1280x720 -> texture 177
[BD-VIDEO] redirect external bind 179 -> GL_TEXTURE_2D 177 (redirect #1..3)
[BD-VIDEO] video draw #1 program=33 unit_2d=[176 0 0 0] unit_ext=[179 0 0 0] backing=177
```

即：我们按 external bind 把 177 绑到 unit 0，**但 Unity 自己的 GL state cache 认为
unit 0 上应该是它自己的 176（它给 `SurfaceTexture` 用的纹理名），在画之前又绑了
回去**，把重定向顶掉。shader 已经被改写成 `sampler2D`，于是它采样的是那个永远
为空的 176 → 黑屏。

验证手段（两个探针，缺一不可）：

* **渐变探针** `BD_VIDEO_DEBUG_GRADIENT=1`：把上传内容换成「R 沿 x 递增、G 沿 y 递增」
  的高对比渐变。修好之前屏幕全黑；修好之后整屏是左上暗→右下亮的渐变（`look.sh`
  一眼可辨）。这证明「视频四边形采样的是我们的纹理」。
* **RGBA 转储** `BD_VIDEO_DUMP_FRAME=<前缀> BD_VIDEO_DUMP_AT=<帧号>`：把 `submit_i420`
  产出的 RGBA 落成 PPM，用来把「数据错」和「采样错」分开。

修法（`thunks/khronos/gles2.cpp`）：**在绘制点重新断言绑定**。`glUseProgram` 记录
当前程序是不是「由改写过的 shader 编译出来的视频程序」，`glDrawArrays/glDrawElements
(/Instanced)` 在真正下发前，把 guest 绑过 external 纹理的那些 unit 上的 2D 绑定
重新换回 backing：

```
[BD-VIDEO] re-bound backing at 1 unit(s) before video draw (#1)
```

修完的效果（容器实测，同一份日志目录）：

| 帧 | 修前 | 修后（渐变探针） | 修后（真实视频） |
|---|---|---|---|
| intro 期 | `mean=1 sd=15 centre=0`（全黑） | `mean=76 sd=21`（整屏渐变） | `mean=200 sd=37 centre=189` |

### 4.1 视频「只显示左下部分」= `textureMaxDim` 缩小了游戏的视频 RenderTexture

掌机（640x480）上视频"全屏显示原视频左下部分"，而编辑器/Android 真机正常。
原因是 `[gpu] textureMaxDim = 512`：它挂在 `bd_glTexStorage2D()` 上缩放 RGBA8 纹理，
只豁免「LUT 形状」和「尺寸**正好等于屏幕**」的纹理，而游戏建的是
`new RenderTexture(1280, 720, ...)` → 1280x720 ≠ 640x480 → 被缩成 **512x288**。
Unity 的 `glViewport` 仍是 `[0 0 1280 720]`，于是只有四边形的**左下 40% × 40%**
落进 RT，UI 再把它铺满屏幕。

判据（日志一眼可辨）：
`video blit target attachment 0 is object 25 size=512x288 viewport=[0 0 1280 720]`
—— attachment 尺寸与它自己的 viewport 不一致，比值 = `textureMaxDim / 长边`。
**修法（已实测）**：掌机 `unity.toml` 设 `[gpu] textureMaxDim = 0` → 同一行变成
`size=1280x720 viewport=[0 0 1280 720]`，画面完整；峰值 `rss≈402MB`，与开着 cap 时
（330~430MB）同量级。完整分析见 [`CASE_STUDIES.md`](CASE_STUDIES.md) 案例三。

### 4.2 顺带修掉的两个坑

* **`libswscale` 不能用来做 I420→RGBA**。同一个转换，掌机的 `libswscale.so.5` 接受，
  ubuntu 20.04 的那个直接拒绝：`No accelerated colorspace conversion found from
  yuv420p to rgba.` → `sws_scale failed`；而掌机上同一个库还在负 stride 上 SIGSEGV。
  现在 `bd_video.cpp` 用自己写的 `i420_to_rgba()`（BT.601 整数式，顺手把垂直翻转
  合并进去），不再有版本差异，也去掉了那次 in-place 翻转。
* **`log_gl_error()`**：Unity 只会打 `OPENGL NATIVE PLUG-IN ERROR: GL_INVALID_ENUM`
  而不会说是哪次调用。重定向/上传后会 drain `glGetError` 并按调用点打点（实测这
  两处都不产生错误，那条 INVALID_ENUM 来自 Unity 自己的某次一次性调用）。

---

## 5. 容器 ≠ 掌机（判读差异用）

* GL：容器是 `Mesa 21.2.6 llvmpipe`（GLES 3.2），掌机是 Mali。`GL_OES_EGL_image_external`
  两边都有，但 llvmpipe 走的是软件路径。
* **UnityShaderCache 是关键变量**：Unity 会把编译好的程序缓存进 `cache/UnityShaderCache`。
  命中缓存时**根本不调 `glShaderSource`**，我们改写的 shader 也就没机会生效
  （日志里 `shader N: samplerExternalOES -> sampler2D` 一行都不会出现）。
  2026-09-16 起 loader 自己管这件事：`bd_shader_cache::prepare()` 给每个缓存目录写
  `.bd_rewrite = <BD_SHADER_REWRITE_VERSION>`，戳不匹配就整目录丢掉重编，日志：

  ```
  [BD-SHADER] shader cache ./assets/bin/cache/UnityShaderCache: found in the port tree -> dropped 1 entries, stamp=2
  ```

  所以现在通常**不需要**手动删缓存；只有在查「旧 loader 写的缓存」时才手动删：

  ```bash
  rm -rf "$GAME/cache/UnityShaderCache"     # Windows: rmdir /s /q ...
  ```

  反过来说，第一次带改写的运行之后，缓存里存的就是**已改写**的程序，后续不再需要删。
  但掌机上如果缓存是旧 loader（没有改写）写的，就会一直黑 —— 排查视频黑屏时先删它。
* **性能**：qemu + llvmpipe 慢很多。容器里 Unity 可能 40 秒只画 2 次视频四边形，
  所以「容器里视频不逐帧动」不能直接判成 bug；判「能不能动」要回掌机看
  `updateTexImage #N` 的计数是否持续增长。
* **Xvfb 竞态**：`run.sh/frames.sh` 已经用 `xdpyinfo` 轮询到 X 真的能连上才启动
  loader。裸 `sleep 2` 在 qemu 下会随机让 `SDL_Init` 失败 → `fatal_error` →
  SIGABRT，看起来像 Unity 崩了。
* **日志量**：`BD_ENABLE_TRACE` 会打开 `[BD-ASSET]`/`[BD-WROPEN]` 等，容器排障可以开，
  上机发布不要开。

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
