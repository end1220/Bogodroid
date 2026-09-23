# Oddmar 移植接手文档（`oddmar` 分支）

> 交接给下一个会话：**先读完本文再动手**。
> 本文分两层读法 —— **§0 → §0.6 → §0.7 → §⛔ → §7.0** 是「现在要干什么」；其余章节是取证与已归档结论，
> 需要时再查。**§0.2 = 遗留问题总表；§0.3 = 全部主要问题的「原因 → 解法」速查**；
> 已定论的部分压缩在 §0.4–§0.5、§4.4、§6（✅ 项）、§10–§12，不必逐字读。
>
> **📌 2026-09-22 晚：黑屏已解决，Oddmar 标题界面已渲染出来**（详见 §0.6-H）。
> 挡画面的 `libunity+0x4c124c`（asset 路径翻译）契约已定稿并在 `BD_BYPASS_VIDEO_TRANSLATE` 里实现。
> **📌 2026-09-22 深夜：L2 定论 —— hostless URL 的 host = `Application.dataPath`**（详见 §0.7）。
> 剩余项见 §0.2 总表，**均不再挡画面**；本轮已收尾提交（版本行见 §13）。
> **📦 资源瘦身（视频 / 贴图）：方案 + 执行记录见 〔[`ODDMAR-ASSET-SLIMMING.md`](ODDMAR-ASSET-SLIMMING.md)〕**
> **2026-09-22 已执行**：视频 **232.5 → 132.8 MB**（854×480 / capped-CRF23 / `-refs 2 -bf 2`，解码像素 **−5.3×**）；
> 贴图转 **ASTC RGBA 8×8**。`astc_retier.py` 两个缺陷已修（裸 `SerializedFile` 崩溃、硬编码 `ASTC_RGB_*` 丢 alpha）。
> 同时修掉 hook 的「**所有视频都指向 splash**」：原始 URL 在 `libunity+0x53f020` 的 **x23** 里
> （**不在** hook 的任何参数中 —— 旧注释说"在 a1"是错的，a1 调用前已被清空），现在按 URL 逐文件映射，见该文 §2.4。
> ⚠️ 18 个关卡过场**只在进关卡时才会被请求**，标题界面验不到。
> **🔊 2026-09-22 深夜（第二轮）：Wwise 初始化已修复** —— 三处独立根因：① `libAkSoundEngine.so` 的
> `JNI_OnLoad` 从未被调用（JavaVM 因此没缓存）；② `UnityPlayer.currentActivity` 用
> `Landroid/app/Activity;` 查不到（描述符注册的是更具体的派生类签名）；③ `AAssetManager_openDir` /
> `AAssetDir_close` / `AAsset_seek` 实现了却没进 thunk 符号表。修完 `Sound engine initialized successfully.`，
> 1493 条逐帧 `RenderAudio` warning 归零。**详见 §0.8**。
> 仍剩：**没有声音** —— Wwise 的音频输出后端要 OpenSL ES，而 port 的 `opensles.cpp` 默认关闭（为了让
> libunity 的 FMOD 绕开 OpenSL 走 SDL）。`LoadBank` / `.bnk` 引用均为 0 ⇒ 147 MB `SoundBanks/` 是否死重量**仍未验证**。

> ## ⚠️ 目标约束（2026-09-21 用户确认，优先级最高）
>
> **Oddmar 只需要『断网单机运行』** —— 不接 Google Play 账号、不做内购、不用在线服务。
> 直接推论，动手前先读这一条：
>
> - **billing / 网络 / 云服务相关的缺失桩一律不修**，只要求「不阻塞启动、不致命」：
>   `com/mobge/oddmarbilling/OddmarPurchaseHandler`、Firebase / Crashlytics、
>   Google Play Services、`AudioManager.isBluetoothA2dpOn`、`DisplayManager.registerDisplayListener`
>   等，全部降级到最末。
> - 排查顺序固定为：**资源加载 → 场景挂载 → 画面**。任何"这条路需要联网"的结论都不成立，
>   先假设它是本地资源问题。
> - 网络类调用失败若**升级成致命异常**（NRE 穿透到启动流程），就把它改成"返回安全默认值"，
>   而不是去实现它的功能语义。

---

## 0. 当前状态与遗留问题

> **最后一次更新：2026-09-22 深夜 · 第三轮（Wwise 出声）**（分支 `oddmar`）
> —— **黑屏已解决（§0.6-H）**、**L2 已定位（§0.7）**、**Wwise 初始化已修复（§0.8）**、
> **Wwise 出声链路已打通（§0.9）**：标题界面既出画面也出 PCM。
> ⚠️ **跑测必带 `BD_BYPASS_VIDEO_TRANSLATE=1`，否则必然黑屏——见 §1.4-0，这是 opt-in 行为不是故障。**

### 0.1 状态速览

| 项目 | 状态 |
|------|------|
| 进程存活 | **60 s 跑满**，无 `[BD-ANDROID] CRASH`、无 `[BD-SEGV]`、无 `terminate`、无 `NullReferenceException` |
| 启动链 | ✅ 越过云/计费阻塞 → `init time: 1` → **8 个 AssetBundle 真实加载**（`Unable to read header` = **0 条**） |
| NDK 视频通路 | ✅ 已启用，且**整条链打通**：`extractor opened tracks=2` → H.264 + AAC 解码器 → `decoded frame=0/1` |
| 片头视频 | ✅ **打开并解码成功**（`fd=30 offset=0 length=979746`，1920×1080 H.264）+ `[BD-VIDEO] swap … uploaded=1` |
| `frame.*.gl.ppm` | 921 615 B（640×480 RGB）—— **不再是 192 B 纯色** |
| X 根窗口截图 | 19 帧 192 B → 渐增 → **稳定 290–300 KB**（判据 ≥180 KB） |
| **画面内容** | ✅ **Oddmar 标题界面**：logo + 森林/维京人/篝火场景 + "Press any button to continue" |
| **音频（Wwise → OpenSL → SDL）** | ✅ **出声链路已打通**（§0.9）：`Sound engine initialized successfully.`、`Silent mode` **0 次**、`Enqueue` **218** 次且 `queued` 在 12.5–15.6 KB 间波动（设备在消费 = 真实 PCM 在流） |
| **音频混音器（视频音轨 + 音量键）** | ✅ **2026-09-23 第四轮**（§0.12）：旧实现"两个引擎抢一个 SDL 设备、谁抢到谁独占"导致 **① 视频音轨无声音 ② 音量键对 Wwise 音乐零作用**。已改为 `audio_bus` 内**软件混音**（`push` + `pump`，统一施加系统增益）。容器两轮验收：两路 `pushed` 实时增长、`peak(1s)` 精确复现"视频=FMOD / 界面=Wwise"、`gain=50%` 随 sysfs 生效。**上机听感待验** |
| `eglSwapBuffers` | 帧循环在跑（`Choreographer$FrameCallback.doFrame` ≈21 Hz） |
| **内存（标题界面）** | ✅ 容器稳定态 **666 MB → 583 MB**（−12.5%，纯 env，§0.10）；**真机实测 341 MB**（运行 2.5 min 后 465 MB，§0.11-E）。**进关卡后的峰值仍未测** |
| **真机（掌机）** | ✅ **2026-09-23 已部署并跑通**（§0.11）：`/mnt/mmc/Roms/ports/Oddmar/` @ `172.16.6.77`（H700 类 / Mali-G31 / 996 MB 无 swap）。判活九项全 0、`redirect external bind`=3、`Silent mode`=0、`Enqueue`=154、GLES 3.2 真驱动；画面 = **开场过场动画** |
| 游戏是否 quit | 未走 Unity 正常 Quit（末尾 `Caught signal, fast-exiting via _exit` 是 `timeout -s INT` 到点） |
| hostless URL（L2） | ✅ **已定位**：host = `Application.dataPath`（空串）。上游 `Context.getPackageCodePath()`（port 侧 `bd_compute_source_dir()`）返回的 `<cwd>/UnityDataAssetPack.apk` 不存在 ⇒ 被 Unity 的 `stat()` + `S_IFREG` 闸门挡下 ⇒ `dataPath=""`。**不挡画面**（§0.7） |

**一句话**：**黑屏已解决。** 挡画面的是一条"asset 路径翻译"链 —— Unity 把
`jar:file://!/assets/Videos/…mp4` 交给 `libunity+0x4c124c` 翻译成本地路径，翻译失败 → `-10004` → 没有片头视频 → 黑屏。
该函数的契约现已定稿（`a1` = 输入/输出的路径 string；`a2` = 数据基址，**NULL 表示数据在文件里**；`a3` = 数据总长；返回 bool），
按此实现的 hook（`BD_BYPASS_VIDEO_TRANSLATE=1`）实测让 extractor 打开 2 条轨、解出 H.264 帧、
**渲染出 Oddmar 标题界面**（§0.6-H）。剩余项见 §0.2 总表，均不再挡画面。

### 0.2 遗留问题总表（🔴 = 挡画面 / ⚠️ = 未查清 / ⚪ = 收尾）

| # | 遗留问题 | 现状 | 指向 |
|---|---------|------|------|
| ~~**L1**~~ ✅ | ~~片头视频翻译链断 → 黑屏~~ **（2026-09-22 晚已解决）** | `libunity+0x4c124c` 的契约已定稿并实现：**回填 a1 路径 + a2 = NULL + a3 = 长度 + 返回 bool 1**。实测 `offset=0`、`tracks=2`、解出 H.264 帧、截图稳定 290–300 KB，**画面 = Oddmar 标题界面** | §0.6-C/D/E/H；验收 §7.0-0a |
| ~~**L2**~~ ✅ | ~~`jar:file://!/assets/...` 的 host 为空，**来源仍未定位**~~ **（2026-09-22 深夜已定位，§0.7）** | host = **`Application.dataPath`**；上游 = `Context.getPackageCodePath()`（port 侧 `bd_compute_source_dir()`），被 Unity 的 **`stat()` + `S_IFREG` 闸门**挡下 —— 返回的 `<cwd>/UnityDataAssetPack.apk` 在磁盘上不存在 ⇒ 候选全废 ⇒ `dataPath=""`。已实测：放一个真实文件即让 `dataPath` 恢复正常（但会连带**打死启动**，见 §0.7-A⑧） | §0.7（定论 + 根治可行性评估）；il2cpp 探针**已接线**（`BD_PROBE_APPPATHS=1`） |
| **L3** ⚠️ | `0x4c124c` 背后的实体类型（`[a0+0x410]` → `vtbl[0x138]`）未摸清；另外 3 个调用点是否同链未确认 | 未做（长期保留该 hook 时才必要） | §7.0-2 |
| **L4** ⚪ | 云服务回调不触发：`SocialImpl.authenticate` / `SaveGames.isConnected` 类未注册 | 不崩、不卡；按目标约束**只需不阻塞** | §0.5-F |
| **L5** ⚪ | 回归未补：本批 `libjnivm` + `thunks/ndk/*` + `getPackageCodePath` 改动 | 早一批 4 组对照已过；本批未跑 | §7.0-7 |
| **L6** ⚪ | `AssetLocator.GetReaderWrapper` 仍是探针（返回默认值，内容未真实读出） | 不阻塞当前主线（8 个 bundle 已打出真实大小） | §6 P0 |
| **L7** ⚪ | 清理类：§4.2 诊断日志、过期注释 `android_content.cpp:934`、探针生命周期 | 容器清理**已完成**（2026-09-22 收尾）；其余逐条见右列 | §7.0-6/8、§7.1-9/10、§9.3 |

> **执行步骤**：L3–L7 的做法与前置条件见 **§7.0**（按优先级分组）。L1 / L2 已关闭，原因与解法见下一节。

### 0.3 主要问题：症状 → 根因 → 解法（速查）

> 本节是**唯一的结论汇总**。新会话读完这里 + §⛔ 接手第 0 步，就能直接开工；
> 每条都指向完整取证（§0.5–§0.7 / §10–§12）。

| 症状（日志里能看到的） | 根因 | 解法（已落地） | 详见 |
|---|---|---|---|
| 启动即退出；`[BD-SEGV] signal 11 si_addr=0xffffffffffffff80`，pc = `std::rethrow_exception` | Unity 的受管异常经 `JNIEnv::Throw()` 进来，jnivm 只把它置成 **pending**（`except` 为空）；下一次 JNI 调用在 `j2invoke` 尾部 rethrow **空 `exception_ptr`** → 读 `-0x80` | `jnivm::RethrowThrowable()`：`except` 为空时抛普通 `std::runtime_error`；`j2invoke` 仅在 `except` 非空时 rethrow；`Throw()` 保持 pending。**另修 SEGV 回溯**：改用 `SA_SIGINFO` 从 Unity 链式 handler 里取真 `ucontext`（**裸 backtrace 会骗人**） | §10 |
| 音频线程崩；`[BD-DBUF] GetDirectBufferAddress handle=0x4`，同一个 `Method*` 两次打印出**不同**的 `native` | jnivm 的 `jmethodID` 就是裸 `Method*`；`UnregisterNatives()` 把它从 `methods` 里 `erase` → 最后一个 `shared_ptr` 释放 → **同尺寸分配复用同一块内存** → 缓存的 id 悄悄指向另一个 native 函数 | `RegisterNatives()` 增加**进程级 keepalive 表**（注册给原生代码的方法永不释放）；`fakefmod.cpp` 改为每次调用重新解析并**校验 name/signature** | §11 |
| hook 装上了、也触发了，但 Unity 仍走失败分支；`could not translate` / `-10004` | `libunity+0x4c124c` 是**虚调用转发 thunk**，真实契约是 `bool f(obj, string* in_out, void** out_base, size_t* out_len)`；原来返回**指针** → 调用点 `tbz w0,#0` 判 bit0 → 对齐指针低位恒 0 → **恒判失败** | hook 改为：**回填 `a1` 路径 + `a2 = NULL` + `a3` = 长度 + 返回 `1`**（`BD_BYPASS_VIDEO_TRANSLATE=1`，实现见 `bypass_video_translate`） | §0.6-C/D/E、§12 |
| **黑屏**：8 个 AssetBundle 加载成功、`eglSwapBuffers` 在跑，但画面恒 192 B 纯色 | 上一条的直接后果 —— 片头视频路径翻译失败 ⇒ 没有片头 ⇒ 黑屏 | 同上（v4 实现）。验收链：`extractors opened tracks=2` → `decoded frame=0/1` → 截图 **290–300 KB = Oddmar 标题界面** | §0.6-E/H、§7.0-0a |
| `streamingAssetsPath = 'jar:file://!/assets'`（host 为空） | host = **`Application.dataPath`**；候选来自 `Context.getPackageCodePath()`（port 侧 `bd_compute_source_dir()`）= `<cwd>/UnityDataAssetPack.apk`，**磁盘上不存在** ⇒ 被 Unity 的 `stat()` + `S_IFREG` 闸门拒 ⇒ `dataPath=""` | **不修**（不挡画面）。已定位 + 探针接线（`BD_PROBE_APPPATHS=1`）。根治需占位 APK + 改跨端口共享的 `clean_jar_path`，收益低 ⇒ 保持 opt-in hook | §0.7 |
| 启动期 `NullReferenceException`，栈落在 `_AndroidJNIHelper.GetSignature` | ① `getConstructorID` 对未绑定 `<init>` 的类返回 **null**（Java 的 `new` 从不返回 null，托管侧拿它当参数就解引用 null）；② 自造类没补 `java/lang/Object` 父类 ⇒ `getClass()/toString()` 全 miss 返回 null | ① 合成惰性活对象（逃生开关 `BD_CTOR_FALLBACK_NULL=1`）；② 在 `InternalFindClass` **函数出口**补父类（**不能**写在 fallback 分支里） | §0.4、§4.4 |
| NDK 视频符号链不完整（`AMediaExtractor_*` / `AMediaCodec_*` 等） | 端口侧缺少完整的 NDK 媒体符号 | 已补齐，机制可复用 | §0.5-A |
| ✅ 已解决（2026-09-22 深夜）**无声**：`Hardware audio subsystem stopped responding. Silent mode is enabled.` | **sink 选路三连**：① port 的 `dlopen_impl` 对未注册库返回**非 NULL 哨兵** `0xDEAD` ⇒ Wwise 的 `return (dlopen("libaaudio.so") != NULL)` 误判 AAudio 可用 ⇒ 27 个 `AAudioStream_*` dlsym 全 miss；② 回退 OpenSL 后 shim 缺 `SL_IID_BUFFERQUEUE` ⇒ `Object_GetInterface` 重试 156 次；③ SDL 2.0.10 **单输出设备**已被 FMOD 占住 ⇒ `SDL_OpenAudioDevice` 必失败 | ① `libaaudio.so` 返回 `NULL`（逃逸 `BD_AAUDIO_SENTINEL=1`）；② shim 补 `SL_IID_BUFFERQUEUE`；③ 新增 `platform/common/audio_bus.*` **共享设备 + FMOD 让路 + 48k→24k 重采样**；④ `dlsym_impl` **按调用者模块分派**（只放行 `libAkSoundEngine`，逃逸 `BD_OPENSLES_OFF=1`） | §0.9 |

### 0.4 历史修复（早期推进，保留备查）

1. **`getConstructorID` 对未绑定 `<init>` 的类合成惰性活对象** —
   `projects/unityloader/javastubs/unity.cpp`。Java 的 `new` 从不返回 null；返回 null 会让托管侧
   把它当参数传给下一个 `AndroidJavaObject`，Unity 的 `_AndroidJNIHelper.GetSignature(obj)`
   解引用 null 抛 NRE。逃生开关 `BD_CTOR_FALLBACK_NULL=1` 恢复旧行为。
2. **自造类补 `java/lang/Object` 父类** — `libjnivm/src/jnivm/internal/findclass.cpp` 的**函数出口**。
   `GetMethodID` 只沿 `cur->baseclasses` 走继承链，而 `InternalFindClass` 新造的类没有 baseclasses
   → `Object.getClass()/toString()` 全 miss 返回 null → 同样砸在 Unity 的 `GetSignature` 上。

> ⚠️ 补父类的代码**必须写在 `InternalFindClass` 出口**，不能写在 `vm->classes.find()` 那个 fallback 分支里：
> `JNIVM_ENABLE_DEBUG` 恒 ON ⇒ `JNI_DEBUG` 已定义 ⇒ 未注册的类走的是 `#ifdef JNI_DEBUG` 的**命名空间分支**，
> fallback 分支根本不执行。第一版写在 fallback 里，编译通过、跑起来毫无效果。

---

## 0.6 第三场（2026-09-22）：分支 `ddmar` + 视频翻译 hook 的 ABI 定论

> 本场**没有解决黑屏**，但把"为什么这个 hook 一定不生效"钉死了：**打的是一个虚调用转发 thunk，
> 它的契约是 `bool` + 两个输出槽，而现在返回的是指针。** 同时更正了上一场三条既有结论。

### A. 分支与提交

- 新分支 **`ddmar`**（从 `video` 切出；该名已随 09-22 目录/分支重排并入 **`oddmar`**）。两个提交：`beab38d` *oddmar: continue offline video startup fixes*（34 个文件）、
  `eb68cf7` *oddmar: add opt-in Unity video translation bypass*。
- 改动面：`javastubs/android*.{h,cpp}`、`javastubs/bd_assetlocator.cpp`、`javac.cpp`、`libjnivm` 三处
  （`method.cpp`/`vm.cpp`/`findclass.cpp`）、`thunks/ndk/media*` + `ndk.cpp`、`thunks/libc/{fcntl,misc,stdio}.cpp`、
  `platform/common/debug_utils.*`、`projects/unityloader/main.cpp` + `javastubs/*`、`scripts/container-run-game-seq.sh`。

### B. 本场新增的两个"离线视频绕过点"

**B-1. `clean_jar_path()` 增加 `<cwd>/gamedata/assets` 映射**（`thunks/libc/fcntl.cpp:82-101`）——
通用清理把 hostless 的 jar 前缀映射成 `<cwd>/assets/...`，新增块在其上找 `/assets/`，若
`<cwd>/gamedata/assets/...` `stat` 存在就改返回它。

⚠️ **实测本轮一次都没被行使**：本场日志里对视频文件**零 `stat`/`open`**。
原因是 Unity 的翻译步骤发生在**文件层之前**就返回了"失败"（见 D）。它是"备用"，
只有 Unity 真去开文件时才有意义。

**B-2. `BD_BYPASS_VIDEO_TRANSLATE=1`：hook Oddmar `libunity.so + 0x4c124c`**

- 实现：`projects/unityloader/main.cpp:34-60`（`bypass_video_translate`）+ `main()` 里 `~1166` 处
  `hook_address_detour(&lunity, addr_lunity + 0x4c124c, …)`，**只在环境变量存在时装**，默认路径不受影响。
- 实测触发 **1 次**（与视频翻译被调用次数一致），路径也打印对了
  （`/game/Oddmar/gamedata/assets/Videos/mobge_and_senri_splash_video.mp4`，文件确实存在 979 746 B），
  **但 Unity 依旧报 `could not translate` → `-10004`**。

### C. 🔴 核心结论：`0x4c124c` 是"虚调用转发 thunk"，而它返回 `bool`

取证对象：`/game/Oddmar/gamedata/lib/arm64-v8a/libunity.so`，15 287 168 B、stripped、
BuildID sha1 `664018de51c6af87bf397e4c6d80b2f1c154dfa9`。**下面所有偏移只对这个 build 有效。**

**`0x4c124c` 是函数入口**（全仓 `bl 4c124c` 命中 4 处），但它不是"翻译函数本体"，而是把调用转给 `vtbl[0x138]` 的 thunk：

```asm
4c124c  mov  x10, x0
4c1250  ldr  x0, [x10, #0x410]   ; x0 = *(a0 + 0x410)   ← 真正实现对象
4c1254  mov  x8, x3
4c1258  mov  x9, x2
4c125c  mov  x3, x9              ; 参数整体右移一格
4c1260  ldr  x11, [x0]
4c1264  mov  x4, x8
4c1268  ldr  x5, [x11, #312]     ; x5 = vtbl[0x138]
4c126c  mov  x11, x1
4c1270  mov  x1, x10             ; 原 a0 变成第 2 个参数
4c1274  mov  x2, x11
4c1278  br   x5                  ; 尾调用 —— 结果直接还给调用者
```

4 个调用点：`0x53f150`、`0x5452dc`、`0x78b81c`、`0x78d534`。
其中 **`0x53f150` 之后紧跟 `could not translate %s to local file`（行号 327）**，与"hook 恰好只触发 1 次"完全对上。

**调用点的真实契约**（`0x53f138`–`0x53f164`）：

```asm
53f138  str  xzr, [sp, #16]      ; 先把输出槽清 0
53f140  add  x0, sp, #0x48       ; a0 = 栈上的包装对象
53f144  add  x1, sp, #0x480      ; a1 = 输入路径
53f148  add  x2, sp, #0x10       ; a2 = 输出槽①
53f14c  add  x3, sp, #0x18       ; a3 = 输出槽②（与①相邻 8 B）
53f150  bl   4c124c
53f154  tbz  w0, #0, 53f1cc      ; ★ 返回值按 bool 判：bit0 == 0 ⇒ 走失败分支
53f158  ldp  x8, x3, [sp, #16]   ; 成功时把 (sp+0x10, sp+0x18) 当 {ptr, size} 读出
53f160  add  x8, x8, x22
53f164  cmp  x9, x3              ; 再用 size 做边界检查
```

⇒ **返回 bool（`w0` bit0）**，**不是 sret**：调用前 x8 未被设置，而 thunk 自己把 x8 当暂存（`mov x8, x3`）
→ 不可能是"结构体按值返回"。

⚠️ **修正（2026-09-22 晚，读完整个消费者 `0x53f020` 后定稿）**：
最初把 a2/a3 判为"两个输出槽 = {路径 ptr, 路径长度}"，**这是错的**。真实契约是：

| 参数 | 类型 | 语义 |
|---|---|---|
| a1 | `string*` | **输入/输出**。进来装原始 URL；调用**前**被 `106178(a1, NULL, 0)` 清空；helper 必须把**本地路径回填**进去。后续前缀检查与 `open()` 都读它第 0 个 8 字节作 data 指针（为 0 时用 `a1+8` 作内联缓冲） |
| a2 | `void**` | 数据基址。**`NULL` 表示"数据在文件里"**；非 0 时该值会被加进 offset，变成交给 extractor 的**文件偏移** |
| a3 | `size_t*` | 数据**总长**；同时是 `offset + size <= *a3` 这条边界检查的上界 |
| 返回 | `bool` | bit0 为真即"翻译成功" |

**完整消费链**（消费者 `0x53f020`；每一步都有 v2–v4 实测支撑，见 §H）：

```asm
53f084  add  x0, sp, #0x480      ; a1 = &local_string
53f08c  bl   <string assign>     ; a1 <- 原始 "jar:file://!/assets/..."
53f090  ...  "http:" / "https:" / "file:" 前缀检查
53f13c  bl   106178              ; ★ a1 <- (NULL, 0)：调用前把那个 string 清空
53f150  bl   4c124c              ; ← 我们 hook 的点
53f154  tbz  w0, #0, <fail>      ; bool 判定
53f158  ldp  x8, x3, [sp, #16]   ; x8 = *a2 (数据基址), x3 = *a3 (总长)
53f15c  add  x9, x20, x22        ; x9 = size  + offset
53f160  add  x8, x8,  x22        ; x8 = base  + offset
53f164  cmp  x9, x3              ; 边界：offset + size <= *a3
53f16c  b.ls 53f25c              ; → 成功路径
53f308  ldr  x8, [sp, #0x480]    ; ★ 回填后的路径 data 指针
53f318  bl   open@plt            ; open(<本地路径>)          ← v3 实测 fd=30
53f330  mov  x2, x22             ; base + offset
53f33c  blr  x8                  ; [vtbl+136](obj,fd,offset,len) == setDataSourceFd
   或   53f2c4: [vtbl+144](obj, path)                          == setDataSourcePath
```

> 外层 `x20 = size`、`x22 = offset` 是从 `0x53eb80` 一路传下来的读取区间；`mov w1, x22` / `mov w2, w20`
> 对应错误串 `"AndroidVideoMedia OpenExtractor offset(%d)+size(%d)"`（`0xbde8d5`）—— 这两个寄存器
> 就是"offset / size"的**字面证据**。

**运行时入参把上面的推断逐项证实**（本场补跑，`log-args3.txt:9707`）：

```
[BD-MEDIA] video translate args=0x40003a4860d8 0x40003a486510 0x40003a4860a0 0x40003a4860a8
                                0x400008d7d5e0 0x17f 0x2f7374657373612f 0x6d2f736f65646956
```

| 观测量 | 与反汇编的对应 |
|---|---|
| `a0 - a2 = 0x38` | = `(sp+0x48) - (sp+0x10)` ✅ |
| `a1 - a2 = 0x470` | = `(sp+0x480) - (sp+0x10)` ✅ |
| `a3 - a2 = 0x8` | 输出槽①②相邻 8 B ✅ |
| `a6 = 0x2f7374657373612f`（LE 字节 `/assets/`）、`a7 = 0x6d2f736f65646956`（`Videos/m`） | **上一层的残值，不是真参数** → 真参数只有 a0–a3 ✅ |

> 顺带记住这个判据：**a6/a7 里是 ASCII 常量碎片 = 调用者没设这两个寄存器**，
> 凡是 `args` 里出现"可读文本当指针用"，基本都能判定该参数位是空的。

### D. 四次实现的失败史 —— 每一步都被日志钉死

| 版本 | 写法 | 结果 |
|---|---|---|
| **v1** | `return (uintptr_t)path.c_str();` | bit0 == 0 ⇒ `tbz w0,#0` 恒判失败 ⇒ `could not translate` → `-10004` |
| **v2** | 写 a2 = 路径指针、a3 = 路径长度，`return 1` | tbz 过了、`could not translate` 消失，**但 a1 从没被回填** → 下游 `open("")` → `OpenExtractor unable to open , error: 0` → `-10004` |
| **v3** | 回填 a1 = 路径；a2 = 内存缓冲、a3 = 文件长度 | **路径与 open 全对（`fd=30`）**，但 a2 被当成 offset 基址 → `offset=70369750858784`（正是那个堆指针）→ `Invalid data found` → `-10000` |
| **v4** ✅ | 回填 a1 = 路径；**a2 = NULL**；a3 = 文件长度 | `offset=0 length=979746` → `extractor opened tracks=2` → 解出帧 → **画面出来** |

**教训**：`tbz w0,#0` 只说明"返回值是 bool"，**不代表** a2/a3 的语义；"能走到下一行"也不代表参数对上了
—— v2 走到了 `open`、v3 走到了 `open` 成功（`fd=30`），**两者都还是错的**。
参数语义只能靠**读完整个消费者**确认，不能从"报错消失了"反推。

### E. 最终实现（v4，已实测通过）

```cpp
// 真实 ABI：a0 = 包装对象, a1 = string*(输入/输出, 回填本地路径),
//           a2 = void**(数据基址, NULL = 数据在文件里), a3 = size_t*(数据总长)；返回 bool
if (a1) {                                          // ★ 回填路径：v2→v4 的关键一步
    const char** slot = reinterpret_cast<const char**>(a1);
    *slot = g_bypass_video_path.c_str();           // static std::string，生命周期足够
}
if (a2) *reinterpret_cast<void**>(a2) = nullptr;   // ★ 必须 NULL，不能给缓冲地址
if (a3) *reinterpret_cast<size_t*>(a3) = (size_t)g_bypass_video_size;
return 1;                                          // bool true，不是指针
```

- 路径字符串必须**常驻**（`static std::string` 满足；别用临时量）；
- **a2 必须为 NULL** —— 给任何非 0 值都会被当作"文件偏移的基址"；
- a3 = 整个文件长度，同时满足 `offset+size <= *a3` 的边界检查；
- a0 不要碰；a4–a7 是残值，别当参数。
- 实现见 `projects/unityloader/main.cpp` 的 `bypass_video_translate`（注释里留了完整调用链）。

### F. 本节更正的三条既有结论

1. **§0.5-C 的因果链被证伪。** `Context.getObbDir()/getObbDirs()` **已经实现**（在 `beab38d` 里，
   `javastubs/android_content.cpp:797-826`，`Activity` 侧 `android_misc.cpp:175-176`，
   由 `bd_compute_obb_dir()` 读 `[paths] android_obb_dirs`），实测
   `[BD-DATADIR] getObbDir -> /game/Oddmar/gamedata`（`log-args3.txt:668`、`7373`）
   ——**但视频 URL 依旧是 hostless 的 `jar:file://!/assets/...`**。
   ⇒ "obb 返回 null ⇒ host 为空" **不成立**，URL 的 host 另有来源。
2. **§0.5-F 的 `[JNIVM] Invalid Reference, Unexpected Type` 与 `JNIVM_ENABLE_RETURN_NON_ZERO=ON` 无关。**
   本场两次复现证明它由**哨兵 `unity.toml`** 触发（证据见 §⛔ 0-1）。
3. **"已加入的参数日志"在上一场一次都没打出来。** 全仓历史日志 `grep -c "video translate args"` = **0**
   ——那两行是在**最后一次运行之后**才进源码并重编的。本场已补跑拿到真值（见 C）。

### G. 本场实测表

| 轮次 | 配置 | 结果 |
|---|---|---|
| hook-test 04:42 | 正常 toml | `bypassing`（路径为空）；无 args 行 |
| hook-test2/3 04:47 / 04:50 | 正常 toml | 路径拼成 `<cwd>/gamedata/gamedata/…`（cwd 已含 `gamedata`，多拼一级） |
| hook-test4 04:54 | 正常 toml | 路径正确；仍 `could not translate`；40 036 行、`init time`=1、8 bundle 正常 |
| **args1 / args2**（本场） | **哨兵 toml** | **两次完全一致：1 357 / 1 358 行、`init time`=0、`ZZZSENTINEL`=12、`Invalid Reference`×2** |
| **args3**（本场） | 正常 toml + `BD_BYPASS_VIDEO_TRANSLATE=1` | 79 168 行、`init time`=1、`Unable to read header`=0、hook 触发 1 次、**拿到 args 真值**；帧仍全 192 B |
| **seq-hook2**（晚·v2） | 正常 toml + bypass | `could not translate` 消失；仍无画面；`unable to open , error: 0` → `-10004` |
| **seq-hook3**（晚·v3） | 正常 toml + bypass | `open` 成功（`fd=30`），但 `offset=70369750858784` → `Invalid data found` → `-10000` |
| **seq-hook4**（晚·v4） | 正常 toml + bypass | ✅ **`offset=0`、`tracks=2`、`decoded frame=0/1`、截图稳定 290–300 KB**（见 §H） |

**产物留档（容器 `/game/Oddmar/`）**：`log-args1/2/3.txt`、`args1/2/3.out`、`seq-args1/2/3/`、
`unity.toml.sentinel-0922`。

**复现命令（照抄）**：
```bash
docker exec GlES_Dev bash -lc '
  cp /game/Oddmar/unity.toml.bak-rd9 /game/Oddmar/unity.toml     # 先还原（§⛔ 0-1）
  cd /game/Oddmar &&
  GAME_ROOT=/game/Oddmar SEQ_DIR=/game/Oddmar/seq-args3 SECS=60 CAP_MS=500 \
  BOOT_LOADER=/workspace/Bogodroid/build-oddmar-verbose/unityloader \
  BD_BYPASS_VIDEO_TRANSLATE=1 \
  bash /run-oddmar-seq.sh > /game/Oddmar/args3.out 2>&1; echo "exit=$?"'
# 注意：build-oddmar-verbose（容器 09-22 05:12）才含 hook + 参数日志；
#       build-regress 里两个字符串都是 0，跑它拿不到 args 行。
```

**反汇编取证怎么做的（可复用）**：
```bash
U=/game/Oddmar/gamedata/lib/arm64-v8a/libunity.so
objdump -d $U | grep -nE "(bl|b|cbz|tbnz)[[:space:]]+.*4c124c"        # 谁调它
objdump -d --start-address=0x53f140 --stop-address=0x53f170 $U          # 调用点契约
python3 -c "f=open('$U','rb');f.seek(0xbde848);print(f.read(160))"      # 字符串 xref 定位
```
（`.text` 的 vaddr == file offset，本 build 下 `0x4c124c` 两种解释一致，所以 `addr_lunity + 0x4c124c` 直接能用。）

**定位消费者函数入口的实用手法**（stripped so 没有符号名）：
```bash
objdump -d --start-address=0x53c000 --stop-address=0x53f200 $U | grep -E "stp\tx29, x30"   # 找 prologue
objdump -d --start-address=0x53f020 --stop-address=0x53f440 $U                              # 读消费者全文
```

### H. ✅ 结果：黑屏解决，画面出来了（2026-09-22 晚）

v4 实跑（`SEQ_DIR=/game/Oddmar/seq-hook4`、`SECS=60`、`BD_BYPASS_VIDEO_TRANSLATE=1`、正常 toml）：

```
9677:  [BD-MEDIA] extractor new
9685:  [BD-MEDIA] bypass: out path='…/mobge_and_senri_splash_video.mp4' base=(nil) len=979746 -> return 1
9688:  [BD-MEDIA] extractor fd source=30 offset=0 length=979746      ← offset 终于是 0
10345: [BD-MEDIA] extractor opened tracks=2
10367: [BD-MEDIA] track 0 mime=video/avc size=1920x1080
10368: [BD-MEDIA] track 1 mime=audio/mp4a-latm rate=48000 channels=2
10369: [BD-MEDIA] create decoder mime=video/avc codec=27 found=h264
10793: [BD-MEDIA] decoded frame=0 bytes=3110400 pts=0                ← H.264 帧真的解出来了
11018: [BD-MEDIA] decoded frame=1 bytes=3110400 pts=16667
11211: [BD-VIDEO] SurfaceTexture(texture=70) registered (sinks=1)
28421: [BD-VIDEO] YUV GPU path ready: program=20 textures=72/73/74 fbo=2
32361: [BD-VIDEO] swap: sinks=1 active=70 uploaded=1 frames=1/1 drop=0 cb=1 swaps=19
32561: [BD-VIDEO] link program 23 (video) status=1
```

**判读四件套**：`init time`=1、`ZZZSENTINEL`=0、`Unable to read header`=0、总行数 81 077。
**失败标记全部消失**：无 `could not translate`、无 `unable to open`、无 `-10004`、无 `-10000`、无 `[BD-SEGV]`。

**画面**（`seq-hook4/timeline.txt`，第 4 列 = PNG 体积）从纯色一路涨到满：

| 时间 | PNG 体积 | 说明 |
|---|---|---|
| 0–15 s | 恒 **192 B** | 启动期，纯色（与旧结论一致） |
| 15–21 s | 443 → 1 334 B | 开始有变化 |
| 21–25 s | 5 874 B | |
| 25–33 s | 47 k → 92 k → 130 k | 淡入 |
| **33 s 之后** | **稳定 290–300 KB** | **真的有画面**（判据 ≥180 KB） |

`frame.*.gl.ppm` = 921 615 B（640×480 RGB）—— 不再是 192 B。
取回 `0050.png` 目视确认：**Oddmar 标题界面** —— "ODDMAR" logo、森林/维京人/篝火场景、
底部 "Press any button to continue"。

⇒ 这已经不只是"片头视频能播"：**启动链整条走通、场景挂载成功、渲染正常**。
`1920x1080 > 640x480` 只有一条性能提示（`[BD-VIDEO] source 1920x1080 is larger than drawable 640x480`），不影响正确性。

> **注意这是 opt-in 的**：只有 `BD_BYPASS_VIDEO_TRANSLATE=1` 时才装 hook，默认路径不变。
> 它**不是临时绕过**，而是把 translate 的职责补全了（回填本地路径 + 给出数据长度）；
> 更"根治"的做法是查清 hostless URL 的 host 来源 —— 那条已在 **§0.7** 定论（host = `Application.dataPath`）。

---

## 0.7 第四场（2026-09-22 深夜）：L2 定论 —— hostless URL 的 host 来源已定位

> ### 🔴 结论
> **host = `Application.dataPath`**。它由 libunity 的一个静态缓冲 `g_appPathBuf`（`libunity+0xF0B740`）提供，
> 唯一写入者是 `SetDataPathChecked()`；而候选路径来自 **`Context.getPackageCodePath()`**
> （port 侧 = `bd_compute_source_dir()`），**被 Unity 的 `stat()` + `S_IFREG` 闸门挡下** ——
> 返回的 `<cwd>/UnityDataAssetPack.apk` 在磁盘上**不存在**，候选全部作废，`dataPath` 留空，
> 于是 `streamingAssetsPath = "jar:file://" + "" + "!/assets"` ⇒ **host 为空**。

### A. 完整证据链（每环都有静态与运行时两侧证据）

**① URL 构造函数（静态）** —— `libunity+0x47e940`，唯一调用点 `0x2fd1dc`：

```asm
47e95c  add  x8, sp, #0x8
47e960  bl   39f800            ; ★ F() —— 取 dataPath
47e964  adrp x0, b76000
47e968  add  x0, x0, #0xc52   ; "jar:file://"
47e974  bl   141734            ; tmp = "jar:file://" + F()
47e978  adrp x1, bc4000
47e97c  add  x1, x1, #0x5b8   ; "!/assets"
47e988  bl   1416bc            ; ※ 结果写进 sret —— 本函数对 F() **无任何校验**
```

**② F() 是虚调用（静态 + 运行时吻合）**

```asm
39f800  mov x19, x8            ; sret
39f808  bl  4bbe14             ; x0 = *(libunity + 0xF0B88)   单例指针
39f814  b   4bfbe8             ; 尾调用
4bfbe8  ldr x0, [x0, #8]       ; obj   = *(singleton+8)
4bfbec  ldr x9, [x0]           ; vtbl  = *obj
4bfbf0  ldr x1, [x9, #368]     ; ★ fn = vtbl[0x170]
4bfbf4  br  x1
```

运行时实测（`BD_PROBE_APPPATHS`）与静态推断**逐位一致**：

```
[BD-L2] [video] singleton@0x3800f0bb88 =0x40000bf7ff70  obj=0x40000bf87fd0
                 vtbl=0x3800e5c068  vtbl[0x170]=0x380030c704 (libunity+0x30c704)
```

（静态侧：`.data.rel.ro` 中 `0xe5c068` 那一族的槽 `+0x170` 重定位正是 `0x30c704`。）

**③ `vtbl[0x170]` 的真身 = 读一个静态缓冲（静态）**

```asm
30c704  mov x19, x8            ; sret: 返回 std::string
30c714  str xzr,[x19] / strb wzr,[x19,#8] ...   ; 构造空 std::string(SSO)
30c724  adrp x20, e8b000
30c728  ldr  x20, [x20, #3080] ; ★ x20 = *(libunity+0xE8BC08) —— GOT 槽
30c730  bl   strlen@plt
30c748  b    std::string::assign
```

`libunity+0xE8BC08` 落在 **`.got`**，其 `R_AARCH64_RELATIVE` addend = `0xF0B740` ⇒
槽里装的是 **`libunity+0xF0B740` 这个 `.bss` 缓冲的地址**。故：

```cpp
std::string vtbl_0x170() { return std::string(g_appPathBuf); }   // g_appPathBuf @ libunity+0xF0B740
```

**④ 谁写这个缓冲：唯一写入者被 `stat()` 把关（静态）**

```asm
4bfc00  <拷贝，上限 0x410>      ; x0 = g_appPathBuf, x1 = src
39f818  ... bl 4bbe14 ... b 4bfc00   ; SetDataPath(string) —— 全仓唯一调用者 = 0x31c520
31c4cc  SetDataPathChecked(p):
31c4e8    bl 31c56c            ; ← 校验
31c520    bl 39f818            ; 通过才写 g_appPathBuf
31c56c  CheckPath(p):
31c57c    bl stat@plt          ; ★ 路径必须存在，否则直接 false
31c590    log "[VFS] Mount %s\n"
31c59c    cmp (st_mode & 0xF000), #0x8000   ; ★ 必须是普通文件 (S_IFREG)
31c5b0    bl 315b30 (查拒绝表) … 失败则 log "Unable to open/read zip file!\n"
```

**⑤ 候选路径来自 `getPackageCodePath()`（运行时）** —— 日志里紧挨着的两行就是铁证：

```
660: [BD-DATADIR] getPackageCodePath -> /game/Oddmar/gamedata/UnityDataAssetPack.apk
661: [NATIVE] stat(/game/Oddmar/gamedata/UnityDataAssetPack.apk)      ← Unity 拿到就 stat
```

**⑥ 探针直接问 Unity（运行时，本轮接线后实测）**

```
[BD-PATH-PROBE] [post-init] Application.dataPath          = ''
[BD-PATH-PROBE] [post-init] Application.streamingAssetsPath = 'jar:file://!/assets'
[BD-PATH-PROBE] [post-init] Application.persistentDataPath  = '/game/Oddmar/conf/'
```

**Unity 自己就把 `streamingAssetsPath` 报成 hostless** —— 说明问题不在 URL 拼接，而在 `dataPath` 本身为空。
全仓 `grep -c "\[VFS\] Mount"` = **0**：没有任何候选路径通过过 `stat`。

**⑦ 决定性实验 E1：放一个真实普通文件，`dataPath` 立刻变正常（运行时）**

```bash
printf "BD-APK-PLACEHOLDER\n" > /game/Oddmar/gamedata/UnityDataAssetPack.apk
```

```
[BD-PATH-PROBE] Application.dataPath          = '/game/Oddmar/gamedata/UnityDataAssetPack.apk'
[BD-PATH-PROBE] Application.streamingAssetsPath = 'jar:file:///game/Oddmar/gamedata/UnityDataAssetPack.apk!/assets'
```

⇒ 返回的正是 `getPackageCodePath()` 的值，**⑤ 与 `stat` 闸门的因果关系就此坐实**。

**⑧ E1 的副作用：启动直接死（运行时）** —— 1372 行、Unity 弹错误对话框（
`AlertDialog$Builder` + `RunOnUiThread` + `Invalid Reference, Unexpected Type`），死因：

```
[NATIVE] stat(/game/Oddmar/gamedata/UnityDataAssetPack.apk/assets/bin/Data/globalgamemanagers)
```

`dataPath` 一非空，Unity 就改从 **APK 路径内部**找数据档 ⇒ 假 APK 里没有 ⇒ 启动失败。
⇒ **`dataPath=""` 反而是当前"能跑"的前提之一**；单纯把它填上等于把黑屏换成死机。

**⑨ 为什么 port 既有的 jar 重映射救不回来（静态）**
`thunks/libc/fcntl.cpp: clean_jar_path()` 的 needle 是

```c
const char* needles[] = {"jar:file:/!", "jar:file://!"};
```

**只匹配无 host 的形式**。带 host 的 `jar:file:///abs/x.apk!/assets/…` 二者都不命中 ⇒
`prefix_at_start = 0` ⇒ 后面那段"映射到 `<cwd>/…/assets`"的重写块（以 `prefix_at_start` 为条件）
**根本不会执行**。这就是 ⑧ 里 `stat` 没被改写成磁盘路径的原因。

### B. 顺带更正的两条既有说法

| 既有说法 | 修正 |
|---|---|
| §7.0-0b：**"只改 `android_package_code`"的干净 A/B** | ⚠️ **是 no-op**。`bd_compute_source_dir()` 在 staged APK 缺失时走 `android_source_dirs` 分支，最后落到 `logical = <cwd>/UnityDataAssetPack.apk`，与 `android_package_code` 取值无关；**只有新值以 `.apk` 结尾时才会改变返回值**。别再按原方案跑这一轮 |
| §0.5-C / §0.6-F：`getObbDir` 一支已排除 | ✅ 仍然成立。本场补上的是"真正那一支" —— **`getPackageCodePath` → `dataPath`**，且它一直都在被调用（`log:660`），只是**返回值过不了 `stat`** |

### C. 「根治」需要什么（已完成可行性评估，**当前不建议做**）

要让 Unity 自己算出合法 URL、从而去掉 `BD_BYPASS_VIDEO_TRANSLATE`，至少三件事：

1. **让 `dataPath` 指向真实存在的普通文件** —— loader 启动时在 `<cwd>` 自造一个最小合法 ZIP
   （空 ZIP 只有 22 B EOCD，放在游戏目录里当占位 APK）。
2. **扩 `clean_jar_path()`** —— needle 增加带 host 的绝对形式，或对 `…/x.apk!/assets/…` 复用同一套
   `/assets/` → `<cwd>/assets/` 重映射（否则 ⑧ 的数据档 `stat` 仍然失败）。
3. **补回归** —— `thunks/libc/*` 是**跨端口共享**代码，Samurai2 / Maximus2 都要过一遍。

**代价/收益**：第 1 条往游戏目录塞假 APK，第 2 条动跨端口共享代码，**收益只是去掉一个 opt-in hook**。
⇒ **建议保持现状**（hook 已 opt-in、默认路径不受影响）；等有"多端口都要视频"的诉求时再做。

---

## ⛔ 接手第 0 步（不看这四条会白跑一轮）

> 以下四条都是**当前环境的真实状态**，不是建议。

**0-1. 容器里的 `unity.toml` 会被留成"哨兵"状态 —— 开工第一件事是核对它**

哨兵配置必然让启动死在 **1 35x 行**左右，两次跑完全一致（§0.6-G 的 args1/args2）。先核对，再决定要不要还原：

```bash
docker exec GlES_Dev bash -lc 'grep -n "android_package_code\|android_source_dirs" /game/Oddmar/unity.toml'
```

出现 `ZZZSENTINELPACK` 或 `android_source_dirs=[]` ⇒ 是哨兵态，还原（备份就在旁边）：

```bash
docker exec GlES_Dev bash -lc 'cp /game/Oddmar/unity.toml.bak-rd9 /game/Oddmar/unity.toml && \
  grep -n "android_package_code\|android_source_dirs\|android_obb_dirs" /game/Oddmar/unity.toml'
```

期望输出：`android_package_code="./"`、`android_source_dirs=["./"]`、`android_obb_dirs=["./"]`。
哨兵版与备份版的差异**只有两行**（`diff` 实测）：`android_package_code` 被改成 `/tmp/ZZZSENTINELPACK.apk`，
`android_source_dirs` 被清空成 `[]`。

> 哨兵版另存为 `/game/Oddmar/unity.toml.sentinel-0922`（没删，做对照用）。
> 注意 `unity.toml` 与 `unity.toml.bak-rd9` 的 mtime 都是 09-21 14:42 —— **别靠 mtime 判断当前是哪个版本，只能 grep 内容**。

**哨兵态的死法（别再误判成 `JNIVM_ENABLE_RETURN_NON_ZERO=ON`）**：

```
[NATIVE] stat(/tmp/ZZZSENTINELPACK.apk/assets/bin/Data/globalgamemanagers)
[JBRIDGE] RunOnUiThread Running runnable!          ← Unity 在弹错误对话框
[JNIBridge] Invoking native handle … for java/lang/Runnable->run
Expected N5jnivm7android3app31DialogInterfaceOnCancelListenerE
[JNIVM]: Exception with Message `Invalid Reference, Unexpected Type` was thrown
[BD-PREFS] flush_all: wrote 1 dirty file(s)        ← 日志到此为止，之后由 timeout -s INT 收尾
```

典型计数：**1 35x 行**、`init time` = **0**、`Unable to read header` = 0、`BD-ASSETLOC` = 0。
（`Invalid Reference` 是哨兵的连带症状，与缓存项无关，见 §0.6-F 第 2 条。）

**0-2. `BOOT_LOADER` 默认指向旧产物**，每次运行都必须显式传（见 §1.4 第 1 条）。

要跑"视频 hook / `PATH-PROBE` 探针"就必须用 `/workspace/Bogodroid/build-oddmar-verbose/unityloader`
（容器 09-22 11:28，**含 hook + 参数日志 + 探针字符串**）；`build-regress`（09-21 14:31）**三样都没有**。

**0-3. il2cpp 探针：✅ 已接线（2026-09-22 深夜）—— `BD_PROBE_APPPATHS=1` 启用**

原先四处断点**已全部补上**（`projects/unityloader/main.cpp`）：

1. `g_probe_app_paths = std::getenv("BD_PROBE_APPPATHS") != nullptr;`（在 `il2cpp_patch::init()` 里）；
2. `init()` 里已解析 `il2cpp_runtime_invoke`（`so_symbol(lil2cpp, "il2cpp_runtime_invoke")`，**不加入硬性必需集合**）；
3. 早退条件已放宽为 `if (g_n == 0 && g_post_init_n == 0 && !g_probe_app_paths) return;`；
4. 两个调用点：`il2cpp_init_hook()` 里的 `probe_app_paths("post-init")`，以及 `bypass_video_translate`
   开头的 `probe_app_paths_pub("video")`（**这个点更晚、更可靠**；`post-init` 可能偏早）。

另外新增 L2 专用探针 `probe_fs_source_pub()`：解出 `*(libunity+0xF0B88)` → `+8` → `vtbl[0x170]`
并打印成 `libunity+偏移`，用于静态定位 host 生产者（详见 §0.7-A②/③）。
实测输出形如：

```
[BD-PATH-PROBE] [post-init] Application.dataPath = ''
[BD-PATH-PROBE] [post-init] Application.streamingAssetsPath = 'jar:file://!/assets'
[BD-L2] [video] singleton@0x3800f0bb88 =0x40000bf7ff70  obj=…  vtbl=0x3800e5c068  vtbl[0x170]=0x380030c704 (libunity+0x30c704)
```

**0-4. 容器临时目录已于 2026-09-22 收尾清理** —— `seq*` 由 **69 个降到 3 个**
（只留 `seq-hook4` = L1 黑屏证据、`seq-l2` / `seq-l2apk` = L2 证据）；
`/tmp` 下的反汇编中间件（`uni.asm` 96 MB、`unity.text.asm` 119 MB、`relocs.txt`、`xref.py`/`grab.py`/`findg.py`）
与已止损项目的 Skul 历史日志（149 MB）已删；`/game/Oddmar` 里 12 个已被文档结论取代的过程日志
（`log-obb-test*` / `log-hook-test*` / `log-jar-bypass*`，约 58 MB）也已删。
**有意保留**：`log*.bak` 5 个（对照基线）与根目录证据日志，清单见 §9.3。

**0-5. ⚠️ 别在 `/game/Oddmar/gamedata/` 里留下 `UnityDataAssetPack.apk` —— 留了就每轮必死**

§0.7-A⑧ 那个实验的副作用：只要该文件存在，Unity 的 `dataPath` 就变为非空，紧接着它会去
`<apk>/assets/bin/Data/globalgamemanagers` 找数据档 → 假 APK 里没有 → 弹错误对话框 → 启动死在 ~1.3k 行。

```bash
docker exec GlES_Dev bash -lc 'ls -la /game/Oddmar/gamedata/UnityDataAssetPack.apk 2>/dev/null && \
  rm -f /game/Oddmar/gamedata/UnityDataAssetPack.apk'   # 有就删
```

死法特征（与哨兵态不同）：无 `init time` 行、无 `extractor opened`、截图**恒 192 B**，
末尾是 `RunOnUiThread` + `AlertDialog$Builder` + `Invalid Reference, Unexpected Type`，
且日志里能看到 `stat(<cwd>/gamedata/UnityDataAssetPack.apk/assets/bin/Data/globalgamemanagers)`。

**0-6. 🔴 掌机「只有某一个游戏按键不对」先怀疑游戏，不要先怀疑 loader**（2026-09-23，§0.13）

用**掌机真机探针**实测过：本机 `ANBERNIC-keys` 的物理键 → SDL 索引 → 注入 keycode **逐键正确**
（A→304→b0→96、X→307→b3→99、Y→306→b2→100、D-pad 走 hat、音量键不抢键），
且各游戏的 `[input]`/`[input.remap]`/内置映射表/`gamecontrollerdb.txt`/系统 SDL 库**全部相同**。
⇒ 若某个游戏仍不对，**差异一定在该游戏自己的输入层**：查
`gamedata/assets/bin/Data/globalgamemanagers`（`strings` 直读 InputManager 轴表）；
**若一条 `joystick button` 都没有，就是该游戏不通过 InputManager 读手柄**，
它自己的日志会打出 `Unity: controllerType: …`（Oddmar 打的是通用 `GameController`，没认出 Xbox）。

**0-6b. 但同一句"问题依旧"还有第二种死法：游戏没活到能用按键**（2026-09-23，§0.13b）

真的按键错、和**游戏已经被 loader 自己请出去**，在用户嘴里是同一句话。区分只看日志**末尾三行**：
`[BD-EXIT]` + `[BD-PREFS] saved` + `unityloader exited (0)` 连排 ⇒ **不是崩溃，是 loader 的热键退出**。
Oddmar 连续两轮真机都是被 `Start+Select` 热键在按下 Select 的同一帧杀掉（用户根本没测完）。
⇒ **日志要倒着读完**，别只盯着中段的 `kc=` 行；中段再正确，末尾一出事用户看到的也是"还坏着"。

---

## 0.5 第二场（2026-09-21 晚）—— 已归档结论（压缩）

> 本节四条结论：①已完成、②作废、③已证伪、④沿用。原文的过程记录已删，需要时看
> `git log`（本文件 2026-09-21 版）或 `.workbuddy/memory/2026-09-21.md`。

### A. ✅ NDK 视频符号链已补齐（已完成，机制可复用）

补齐前 Unity 明确放弃 NDK 路径（`could not load symbol AMediaFormat_toString, will stop loading NDK.`
→ `NDK-based video playback disabled, will use JNI instead.`）。补法是**逐个试错**，每补一个符号报错就换成下一个。
本轮补入（`thunks/ndk/media.h` / `media.cpp` / `ndk.cpp`，均已在 `symtable_ndk` 注册）：

| 家族 | 补的符号 |
|------|---------|
| `AMediaFormat` | `toString`、`getDouble`、`getBuffer`、`setInt64`、`setFloat`、`setDouble`、`setString`、`setBuffer` |
| `AMediaExtractor` | `unselectTrack`、`getSampleFlags` |
| `AMediaCodec` | `createCodecByName`、`createEncoderByType`、`getInputFormat`、`getBufferFormat`、`releaseOutputBufferAtTime`、`getName`、`setParameters` |
| `AMEDIAFORMAT_KEY_*` | **全集 ~50 个**（原来只有 NDK 头的 15 个），含 Java `MediaFormat` 的扩展键 `MAX_HEIGHT`/`PROFILE`/`GRID_COLUMNS` 等 |

**实测（rd8）**：`could not load symbol` 行消失，`[BD-MEDIA] extractor new` 被调到 → NDK 路径确实启用了。

> **机制备忘**：`dlopen("libmediandk.so")` 返回假 handle `0xDEAD`（`thunks/libc/misc.cpp:72`），
> `dlsym`（`misc.cpp:157`）→ `so_resolve_link`（`loader/so_util.cpp:619`）遍历 `so_dynamic_libraries[]`
> （含 `symtable_ndk`），**名字匹配即命中**。所以往 `symtable_ndk` 加 `NO_THUNK(str, func)` 条目
> 就等于"让 Unity 认为 NDK 可用"。`NO_THUNK` 定义在 `thunks/thunk_gen.h:169`。

### B. ⚠️ 已作废：上一轮的"哨兵 A/B 决定性实验"结论不成立

那次实验**同时改了两个变量**（`android_package_code` → `/tmp/ZZZSENTINELPACK.apk`，
且 `android_source_dirs` → `[]`）。后者会**直接打断启动**（`[BD-ASSETLOC]` 出现 0 次、
`init time` 0 次、日志仅 512 行）。所以 `jar:file://` 错误"消失"**只是因为进程死得更早、压根没跑到视频那一步**，
**这个实验什么都没证明**。

**正确做法**：重做 A/B 时**只改 `android_package_code`**，`android_source_dirs=["./"]` 必须保留。
（已排进 §7.0-0b。`android_source_dirs=[]` 必然致死已由两次复现坐实，见 §0.6-G。）

### C. ⛔ 已证伪：`getObbDir()`/`getObbDirs()` 返回 null ⇒ host 为空

这条曾是"真嫌疑"，现已**完全排除**：两个方法已实现并实测返回 `/game/Oddmar/gamedata`，
**URL 依旧 hostless**。详见 §0.6-F 第 1 条 —— **别再沿着"让 obb 非空"往下推**。

### D. il2cpp 探针：✅ 已接线并已给出结论（原"代码未接线"作废）

探针（tag `PATH-PROBE`）用 `il2cpp_runtime_invoke` 调托管侧
`Application.get_dataPath / get_streamingAssetsPath / get_persistentDataPath / get_temporaryCachePath`，
绕过一切猜测直接问 Unity 自己的值。**接线方式见 §⛔ 0-3**（现在是 `BD_PROBE_APPPATHS=1` 启用）。

> **2026-09-22 深夜实测结论**：`dataPath` 打印出来就是**空串**，且 Unity 自己把 `streamingAssetsPath`
> 报成 `'jar:file://!/assets'`。当时预设的两条岔路走的是**第二条的更深处**：不是"Java 侧供给空"，
> 而是**候选路径存在、但过不了 Unity 的 `stat()` 闸门**。完整链条见 **§0.7** —— 那条"转查
> `0x39f800 → 0x4bfbe8 → 0x4bbe14`"的方向是对的，本场把它走到底了。

### E. 同期发现、但不属于视频线的阻塞

| 现象 | 事实核对 | 处理建议 |
|------|---------|---------|
| 云服务回调不触发 | `SocialImpl.authenticate` / `SaveGames.isConnected` 是 `[STUB-MISS]`、`entries=0`（类未注册）→ 回调查不到。**不是轮询卡死**（各仅 1 次 / 12 次调用） | 断网约束下只需"不阻塞"。若证实它卡住 `MGLOProgressData.construct` 链再修 |
| Wwise 起不来 | **✅ 已结案（2026-09-22 深夜第二轮）**：`Sound engine initialized successfully.`；`AK_Fail` / `Awake() not executed` / `RenderAudio` 逐帧 warning 全部归零。三处根因（`JNI_OnLoad` / `currentActivity` 签名 / `AAsset*` 符号）见 **§0.8** | 已修（音效无声仍需 OpenSL，见 §0.8 末） |
| `[JNIVM] Invalid Reference, Unexpected Type` | **✅ 已结案**：只在**哨兵配置**下出现，与 `JNIVM_ENABLE_RETURN_NON_ZERO` 无关（§0.6-F 第 2 条） |

---

## 0.8 Wwise 初始化修复（2026-09-22 深夜第二轮）

**修复前基线**（`/game/Oddmar/log-seq.pre-wwise.txt`，90 s 跑测）：

| 计数 | 值 | 含义 |
|---|---|---|
| `AKDEBUG: … RenderAudio(): AkInitializer.cs Awake() was not executed yet.` | **1512** | 引擎在跑（否则这行打不出来），但初始化没完成 —— 而且是**逐帧**刷 |
| `AKDEBUG: … PostEvent(…)` 同文 | 17 | 游戏持续 PostEvent，全部空转 |
| `WwiseUnity: Failed to initialize the sound engine. Reason: AK_Fail` | 1 | 最终失败 |
| `Wwise: Android initialization failure.` | 1 | Android 平台特化阶段就失败 |

### 根因 ①：`libAkSoundEngine.so` 的 `JNI_OnLoad` 从未被调用

`so_load()` 只跑 `.init_array`（`loader/so_util.cpp:691` 的 `so_initialize`）。而 Wwise 缓存 JavaVM 的
**唯一**位置就是 `JNI_OnLoad` —— 整个函数只有 5 条指令：

```asm
000000000005c208 <JNI_OnLoad@@Base>:
   5c208:  adrp  x2, 430000
   5c20c:  mov   w1, #0x6
   5c210:  movk  w1, #0x1, lsl #16   ; w1 = 0x10006 = JNI_VERSION_1_6
   5c214:  str   x0, [x2, #1088]     ; ★ 把 JavaVM* 存到全局 0x430440 (.bss)
   5c218:  mov   w0, w1
   5c21c:  ret
```

真机上这一步由 Android linker 在 `dlopen` 时完成；port 用 `so_load` 手动 mmap，**不会**。
⚠️ `libmain` / `libil2cpp` / `libunity` / `libBootstrap` 在 `main.cpp` 里**都已显式补调** `JNI_OnLoad`
（`1471` / `1490` / `1498` / `1316` 行），**唯独 libAkSoundEngine 漏了**。

**修法**（`projects/unityloader/main.cpp`，`load_so_from_file(&lak, …)` 成功后）：

```cpp
auto akJNI_OnLoad = (jint (*)(JavaVM* vm, void* reserved))(so_symbol(&lak, "JNI_OnLoad"));
if (akJNI_OnLoad) akJNI_OnLoad(&vm, nullptr);
```

证据：`[BD-AUDIO] libAkSoundEngine JNI_OnLoad(0x3a0005c208) -> 0x10006 (JavaVM cached)`

### 根因 ②：`UnityPlayer.currentActivity` 用 `Landroid/app/Activity;` 查不到

修完 ① 后 Wwise 前进到「取 Activity」这步，立刻失败：

```
[BD-FINDCLASS]: FindClass(com/unity3d/player/UnityPlayer)
[JNIVM]: Invoked Unknown Field Getter Class=`com/unity3d/player/UnityPlayer`
         Field=`currentActivity` Signature=`Landroid/app/Activity;`
[BD-ANDROID] AKDEBUG: Java VM not initialized or not provided in AkInitSettings.
[BD-ANDROID] CRASH: signal 11 (SIGSEGV), fault addr 0x0000000000000230
```

- jnivm 的字段查找**要求 name 与签名同时相等** —— `libjnivm/src/jnivm/internal/field.cpp:23`
- 而描述符按 C++ 静态成员类型注册成 `Lcom/unity3d/player/UnityPlayerActivity;`
  （`projects/unityloader/javastubs/unity.cpp:1162`）
- ⇒ 精确匹配 miss ⇒ jnivm 另造一个**空的 auto-stub 字段** ⇒ getter 返回 NULL
  （`field.cpp:128-134` 的 `else` 分支就是这条 `Unknown Field Getter`）
- ⇒ Wwise 把 NULL 当 JavaVM 解引用 ⇒ SEGV

**修法**（`projects/unityloader/javastubs/binding.cpp`，`registerClass<UnityPlayer>()` 之后）：
给 `currentActivity` 补一条**共享同一 `getnativehandle`/`setnativehandle`** 的别名条目，签名
`Landroid/app/Activity;`。`UnityPlayerActivity` 本就派生自 `jnivm::android::app::Activity`
（`unity.h:88`），所以两个签名指向同一块存储天然成立。

> ⚠️ 不要把 `currentActivity` 的类型直接改成基类 —— 那会反过来让按
> `Lcom/unity3d/player/UnityPlayerActivity;` 查询的调用者失配。**两个签名都要在。**

### 根因 ③：`AAsset*` 三个符号有实现、没注册

修完 ② 后 Wwise 开始枚举 SoundBank 目录，撞上未解析符号：

```
[BD-SYM] Unknown symbol encountered
[0x3a00058994]                                  ← libAkSoundEngine 内部调用点
../loader/so_util.cpp:370: Unknown symbol "AAssetManager_openDir" (0x3a00427430).
[BD-ANDROID] CRASH: signal 6 (SIGABRT)
```

`thunks/ndk/asset_manager.c` 里 `AAssetManager_openDir` / `AAssetDir_close` / `AAsset_seek` **都有实现**，
但 `thunks/ndk/ndk.cpp` 的 `symtable_ndk` 只注册了 6 个 AAsset 条目 ⇒ 未解析符号被填成 `plt0_stub`
⇒ **首次调用直接 abort**（`so_util.cpp:354-372` 的 `reloc_err` 是硬失败，不是延迟降级）。
已补齐（连 `AAssetDir_getNextFileName` / `AAssetDir_rewind` 一起）。

> 🔎 **通用手法**：`libAkSoundEngine.so` 的 UND 符号 − port 的 `NO_THUNK` 注册集合，一次算全差集。
> 本次 96 条差集里，只有这 3 个是「Android 特有且 host 没有」的（其余是 host libc / zlib 表）。

### 验收（`build-oddmar` = Release + LOG=ON + TRACE/VERBOSE OFF，60 s 跑测，`seq-wwise4`）

| 指标 | 修复前 | 修复后 |
|---|---|---|
| `Sound engine initialized successfully.` | 0 | **1** ✅ |
| `AkInitializer.cs Awake()…` warning | 1512 | **0** |
| `Failed to initialize` / `Android initialization failure` | 1 / 1 | **0 / 0** |
| `RenderAudio` / `PostEvent` warning | 1493 / 17 | **0 / 0** |
| `CRASH` / `Unknown symbol` | 有 | **0 / 0** |
| 四件套（ZZZSENTINEL / Unable / couldnot / InvalidRef） | 0 | **0** |
| 有画面帧 / 最大帧 | — | **29 / 36**，231 027 B |
| RSS hwm | 807.8 MB | **663.4 MB** |

⚠️ **一轮"卡在 EGL 窗口创建、日志仅 646 行"的启动竞态仍需注意** —— 同一二进制复跑即恢复，
与 §0.6 记过的同类偶发一致，与本轮改动无关。

### 仍未闭环

1. ~~**没有声音**~~ ✅ **已于同日第三轮解决，见 §0.9**（当时列出的方向——"在 `dlsym_impl` 里按调用者模块分派"
   和"开 shim 会同时改变 FMOD 选路"——**判断正确，就是按这个做的**）。
2. **147 MB `SoundBanks/` 是否死重量仍未验证**：本轮 `LoadBank` 调用 0 次、`.bnk` 引用 0 次，
   RSS 也没上升 ⇒ 标题界面阶段**不加载任何 SoundBank**（倾向"按需加载"）。要下结论需**进关卡**。

---

## 0.9 Wwise 出声修复（2026-09-22 深夜第三轮）

> 上一轮（§0.8）修好了**初始化**，但逐帧仍打 `Hardware audio subsystem stopped responding.
> Silent mode is enabled.`。本轮把输出链路打通 —— **标题界面已能出声**。
> 验收轮：`SEQ_DIR=/game/Oddmar/seq-audio-verify`、`SECS=75`、**必须带 `BD_BYPASS_VIDEO_TRANSLATE=1`**（§1.4-0）。

### A. 三处根因（全在 port 侧；反汇编 + 有界日志逐层剥开）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| ④ | `Silent mode` 每轮必现 | `libAkSoundEngine.so` 的 sink 工厂（`0x1f39e8` → `0x2278f8`）按 `SDK_INT > 26` **优先选 AAudio**，探测函数 `0x226d4c` 的实体就是 `return (dlopen("libaaudio.so") != NULL)`；而 port 的 `dlopen_impl` 对**未注册库返回 `0xDEAD`（非 NULL）** ⇒ Wwise 误判 AAudio 可用 ⇒ 27 个 `AAudioStream_*` dlsym 全 miss ⇒ 静音 | `thunks/libc/misc.cpp`：`libaaudio.so` 返回 **`NULL`**（逼它回退 OpenSL）。逃逸开关 `BD_AAUDIO_SENTINEL=1` 恢复旧行为 |
| ⑤ | 回退到 OpenSL 后仍静音；`Object_GetInterface` 重试 **156** 次 | shim 缺 **`SL_IID_BUFFERQUEUE`** —— Wwise 的 UND 符号之一，与 `SL_IID_ANDROIDSIMPLEBUFFERQUEUE` 指向同一 bufQ VTbl | `thunks/opensles/opensles.cpp`：补该分支（并加 `bd_iid_name()` 诊断）。实测 `unsupported` 156 → **0**，`slCreateEngine` 重试 156 → **8**→ 最终 **1** |
| ⑥ | `SDL_OpenAudioDevice` 失败 `"Audio device already open"` | **SDL 2.0.10 单输出设备模型**：同一进程第二次开必失败，default / by-name 都一样（已写最小 C 程序实测，故放弃"按设备名重试"）。FMOD 已先占住设备 | 新增 **`platform/common/audio_bus.{h,cpp}`** 音频总线仲裁：`publish / unpublish / device / set_owner / owner`（`BdAudioOwner {NONE, FMOD, WWISE}`）。Wwise **复用 FMOD 的设备** 并 `set_owner(WWISE)`；FMOD 侧 `runAudio()` 发现 owner≠FMOD 就 `sleep_for(5ms); continue;` 让路。另加 **48k→24k 线性重采样**（`bd_resample_s16()`），因为复用到的设备是 24 kHz |

### B. 两处配套改动

1. `CMakeLists.txt`：`BD_ENABLE_OPENSLES_SHIM` 默认 **OFF → ON**（注释重写为 "per-caller dispatch; Wwise only"）。
2. `thunks/libc/misc.cpp::dlsym_impl` 改为**按调用者模块分派** OpenSL 符号：
   `bd_is_opensl_symbol()`（匹配 `slCreateEngine` 与 `SL_IID_*`）+ `bd_caller_is_ak_sound_engine(ra)`，
   后者用新增的 `so_module_containing(__builtin_return_address(0))`（`loader/so_util.{h,cpp}`，遍历所有模块
   的 text/patch/cave/data 段匹配地址）。**只放行 `libAkSoundEngine`** ⇒ libunity 自己的 FMOD 拿不到 OpenSL
   符号 ⇒ 不会改选 OpenSL 而卡住。逃逸开关 `BD_OPENSLES_OFF=1`（拒绝 `slCreateEngine`，等价改动前）。

### C. 验收（`build-oddmar` = Release + LOG=ON + TRACE/VERBOSE OFF，75 s，`seq-audio-verify`）

| 指标 | 修复前 | 修复后 |
|---|---|---|
| `Sound engine initialized successfully.` | 0 | **1** ✅ |
| `Silent mode is enabled.` | 6 / 60 s | **0** ✅ |
| `slCreateEngine` 重试 | 156 | **1** ✅ |
| `Object_GetInterface` unsupported | 156 | **0** ✅ |
| `Enqueue #N` 入队 | 0 | **218** 次，`queued` 在 12 544–15 616 B 间波动 ⇒ **设备在消费 = 真实 PCM 在流动** ✅ |
| 判活四件套（ZZZSENTINEL / Unable / couldnot / CRASH） | 0 | **0 / 0 / 0 / 0**（另有 SEGV / terminate / Unknown symbol / GL error / NRE 全 0） ✅ |
| 画面最大帧 | — | **274 665 B = Oddmar 标题界面** ✅ |

关键日志：

```
[BD-OPENSLES] slCreateEngine -> engine 0x4003440022a0
[BD-OPENSLES] reusing shared SDL dev 2 (24000 Hz x 2 ch x 16 bit); Wwise wants 48000 Hz x 2 ch x 16 bit -> resampling
[BD-OPENSLES] CreateAudioPlayer: 48000 Hz x 2 ch x 16 bit -> SDL dev 2 (queued-depth target 19200 B)
[BD-OPENSLES] SetPlayState(PLAYING) -> resuming SDL dev 2, pump=0
[BD-OPENSLES] Enqueue #1250: 256 B (queued 12544 B)
```

> ⚠️ `SDL_AUDIODRIVER=dummy` 下 `queued` 深度照样波动（SDL 的 dummy 后端也在"消费"），
> 所以它只能证明**数据流成立**，不能证明掌机上真有声音。**上机听声是独立一步。**

### D. 本轮排除的一个假故障 —— 值得记住

15:32–15:43 连续 **11 轮全黑**，且**改动前遗留的旧二进制也全黑** ⇒ 一度判定"环境漂移、
与改动无关"。**结论：不是环境，是我的跑测少了 `BD_BYPASS_VIDEO_TRANSLATE=1`。** 排除链：

- 容器 GL/X 栈**健康** —— 最小 SDL2+GLES2 清屏探针在同一 Xvfb 上正常出帧
  （`GL_RENDERER = llvmpipe (LLVM 12.0.0, 128 bits)`，根窗口截图 209 B / RGB=255,38,13）。
- X 窗口**存在** —— `xwininfo -root -tree` 有 `0x20000e "Teapot"`（WM_CLASS `unityloader`）640×480+0+0，
  但**根窗口与子窗口截图都是 192 B** ⇒ 不是合成问题，是没东西可合成。
- `gamedata/` 自 **12:53** 起未变，而 13:03–14:39 多轮有画面 ⇒ 资源排除。
- 真正的变量是一个**没 export 的环境变量**。日志判据一句话：
  **有画面轮 `[BD-VIDEO] redirect external bind` = 3，黑屏轮 = 0。**
- ⚠️ 这个假故障**无法用 A/B 区分**：换 env 逃逸开关、换显示号、起全新 Xvfb、换旧二进制 —— 全都黑，
  因为变量不在被测对象里。**"换了没变化"不等于"改动无关"。**

---

## 0.10 内存占用分解与优化（2026-09-23）

> 起因：「还没进游戏 RSS 就很大」。**结论：标题界面稳定态 666 MB → 调优后 583 MB（−12.5%）**，
> 且**画面与音频判据无回归**。调优全部是启动脚本里的 4 个环境变量，零代码改动、零性能退化。

### A. 数字（容器内，`build-oddmar`，75 s / 标题界面）

| 时刻 | RSS | 说明 |
|---|---|---|
| so 加载完成（4 个 `.so` mmap） | 57 MB | il2cpp 36 + unity 15 + AkSound 4 + main 0 |
| 第一次 `eglSwapBuffers` 采样 | ~450 MB | 引擎 + 元数据 + 首屏资源已进 |
| 稳定态（标题界面，未进关卡） | **665–666 MB** | 重复两次只差 1 MB |
| 全轮 `hwm` | 689 MB | |

> ⚠️ 别拿单次 689 MB 当基线 —— 它在多轮里只出现一次，稳定值是 666 MB（重复性 ±1 MB）。

### B. 按 RSS 分解（`smaps` 聚合，稳定态 689 MB 那次）

| 归属 | RSS | 备注 |
|---|---|---|
| **匿名合计** | **590 MB** | 占 85.6% |
| ├ `rwxp` 匿名段 | **128 MB** | **宿主地址 → qemu 的翻译缓存（见 §C）** |
| ├ 其余匿名（heap / arena / 大块分配） | ~430 MB | 调优的主要作用对象 |
| └ `libil2cpp` / `libunity` 映射区 | 12 / 15 MB | |
| `libLLVM-12.so` | 36.5 MB | llvmpipe 的 JIT（见 §C） |
| `swrast_dri.so` | 10.9 MB | llvmpipe 光栅化器（见 §C） |
| `global-metadata.dat` | 7.0 MB | il2cpp 元数据，很瘦 |
| `unityloader` 自身 | 5.7 MB | |
| libavcodec / avformat | 4.5 / 2.2 MB | |
| `qemu-aarch64` | 3.0 MB | |
| librsvg / libicuuc / … | 1.9 MB / 0.5 MB | **别被"文件尺寸"骗，见 §F** |

### C. ⚠️ 容器 ≠ 掌机：先扣掉模拟开销

`uname -m` 报 `aarch64`，但 `/proc/cpuinfo` 是 **AMD Ryzen 9 7950X**，宿主 `docker info` 是 **x86_64**；
`ps` 里每条命令都长成 `/usr/bin/qemu-aarch64 <binary>`。**容器是在 x86_64 上做 aarch64 用户态模拟**，
`nproc=32`。于是有两块**掌机上根本不存在**的开销：

| 项 | 大小 | 为什么掌机没有 |
|---|---|---|
| qemu 翻译缓存（`rwxp`，宿主地址 `0x7xx…`） | **128 MB** | 原生 aarch64 不需要翻译 |
| llvmpipe（`libLLVM` 36.5 + `swrast` 10.9） | **~47 MB** | 掌机用 Mali 等真 GPU 驱动 |

⇒ **掌机等效 ≈ 583 − 128 − 47 ≈ 408 MB**（粗估，真机还需实测；掌机侧另有 Mali 驱动占用）。

`nproc=32` 还有两个副作用，都会让容器数字**虚高**：
① llvmpipe 默认按核数起 worker 线程（实测进程共 **106** 线程）；
② glibc 的 arena 上限是 `8 × 核数`，32 核 ⇒ 起步 256 个，实测看到 **101 个 64 MB 对齐的匿名段**。

### D. 实测 A/B（同一二进制，仅 env 不同，容器内 32 s 稳定态）

| 组 | RSS | Private_Dirty | 线程 |
|---|---|---|---|
| 基线（两次） | 665 / 666 | 588 / 589 | 106 |
| `MALLOC_ARENA_MAX=2` | 663 | — | 106 |
| `MALLOC_ARENA_MAX=1`（三次） | 616 / 622 / 616 | 538 / 544 | 106 |
| `LP_NUM_THREADS=2` | 635 | 558 | **50** |
| `MALLOC_ARENA_MAX=1` + `LP_NUM_THREADS=2` | 600 | 522 | 50 |
| `MALLOC_ARENA_MAX=2` + trim 阈值 | 609 | 532 | 106 |
| `MALLOC_ARENA_MAX=1` + trim 阈值 | 605 | 521 | 106 |
| **`ARENA_MAX=2` + trim 阈值 + `LP_NUM_THREADS=2`（采用）** | **583** | **506** | **50** |

解读（**注意别走错路**）：

- **"arena 段数多" ≠ "RSS 高"**。基线有 101 个 64 MB 对齐的匿名段，但把 `ARENA_MAX` 压到 2 之后
  段数掉到 1，RSS 只降 **3 MB** —— 那些段绝大多数是**空的虚拟预留**。看段数会被带偏，只看 RSS/Private_Dirty。
- 真正干活的是 **`MALLOC_TRIM_THRESHOLD_` / `MALLOC_MMAP_THRESHOLD_`**（让 glibc 把释放的堆及时
  还回内核）。单独配 `ARENA_MAX=2` 就能降 57 MB。
- `LP_NUM_THREADS=2` 降 31 MB 并砍掉 56 个线程，但**只在软渲染下有意义**；真 GPU 上不加载 llvmpipe。
- **性能无退化**：各组 32 s 日志吞吐持平（基线 78 237/78 267 行 vs 优化组 78 775/78 390 行）。

### E. 落地

启动脚本 `LinuxArmPorts/oddmar_port_stage/Oddmar.sh`（**新建**，此前的 port 一直缺）里 export：

```bash
export BD_BYPASS_VIDEO_TRANSLATE=1          # 画面，必须（见 §1.4-0）
export MALLOC_ARENA_MAX=2
export MALLOC_TRIM_THRESHOLD_=65536
export MALLOC_MMAP_THRESHOLD_=131072
export LP_NUM_THREADS=2
```

⚠️ **这些变量只能由 shell 设**。glibc 在 `main()` 之前就把 `MALLOC_*` 读走了，
loader 内部再 `setenv()` 已经太晚 ⇒ 不要指望在 `main.cpp` 里设。

### F. 更正一条容易被"文件尺寸"带偏的判断

`libavcodec.so` 会链 `librsvg` → `libicuuc`/`libicudata`、以及 `libcodec2`，**文件尺寸**分别是
9 MB / 27 MB / 14 MB，看起来像"裁掉能省 50 MB"。但**按 RSS 看只加载了几 MB**
（`librsvg` 1.9 MB、`libicuuc` 0.5 MB、`libicudata` 甚至没进 top25；这些是共享库，页在多进程间共享）。
⇒ **判断可裁剪性要看 RSS，不能看 `du`/映射虚拟大小。** 这一项收益远小于 §D 的调优。

### G. 未闭环

1. **掌机上复测** —— §C 的 408 MB 是容器推算值，真机（Mali + 原生 aarch64 + 实际 RAM）必须实测。
2. **进关卡后的峰值** —— 本节的 583 MB 只是**标题界面**。SoundBanks（147 MB）与关卡资源尚未加载，
   进关卡才是内存峰值所在（同时这也是 §0.9 遗留的 SoundBank 死重验证）。
3. `BD-ANY-MISS` 每轮 **60 168** 次 → 大量 JNI 方法在反复走 fallback。**不直接吃内存**，
   但属于性能侧的独立议题，值得另立任务。

## 0.11 掌机部署与真机首跑验收（2026-09-23）

> **结论：Oddmar 已在真机跑通 —— 画面 + 音频 + 判活九项全过，RSS 341 MB（容器 666 MB）。**
> 部署根：`/mnt/mmc/Roms/ports/Oddmar/`，掌机 `172.16.6.77`（agent `dropbeak 0.6.4`）。

### A. 真机环境（实测，不是推断）

| 项 | 值 |
|---|---|
| SoC / GPU | Allwinner H700 类：**4×Cortex-A53 + Mali-G31**（`Mali-G31 1 cores r0p0 0x7093`） |
| 内存 | **996 MB 总 / 无 swap**（`/proc/swaps` 为空）—— 内存是这台的硬约束 |
| framebuffer | `/dev/fb0`：`virtual_size=640,960` `bpp=32` `stride=2560` ⇒ **640×960×4 = 2 457 600 B**（两帧 640×480 堆叠） |
| SDL | **2.0.12**（`libSDL2-2.0.so.0.12.0`）。与容器 2.0.10 同为**单输出设备**模型 ⇒ §0.9 的 `audio_bus` 仲裁在真机同样生效 |
| 挂载 | `/mnt/mmc` = **vfat**（`/dev/mmcblk0p8`，fmask/dmask=0000）。**没有 noexec**，`unityloader` 可直接执行 |
| 前端 | `/mnt/vendor/bin/dmenu.bin` 持有 framebuffer（`/mnt/vendor/ctrl/dmenu_ln` 拉起） |
| 依赖 | `ldd unityloader` **0 个 not found**（SDL2 / FFmpeg 全家桶 / libasound 全在 `/usr/lib`） |

### B. 部署（Dropbeak）

推的是**瘦身版**（§ODDMAR-ASSET-SLIMMING §9 那套，即容器 `/game/Oddmar` 的当前状态），
不是 staging 目录里的原始版 —— ⚠️ `oddmar_port_stage/Oddmar/gamedata` **至今仍是瘦身前的原始版**
（Videos 232 MB），别直接拿它推。

| 目标 | 方式 |
|---|---|
| `unityloader`、`unity.toml`、`Oddmar.sh`、`relaunch.sh`、`fbcap.sh` | `dropbeak-cli push … --force --chunk --chunk-size 16m --verify` |
| `gamedata/`、`conf/`（大目录） | **`tar -cf - <dir> \| curl -X POST .../api/v1/files/extract?path=…`** —— agent 是**流式** `tar.NewReader(r.Body)`，**不支持 gzip**，必须裸 tar；`ReadTimeout` 15 min |

- 容器**能直达掌机**（HTTP 200，RTT 80 ms）⇒ 省掉"容器 → 本机 → 掌机"中转。
- 实测速率 **≈3.8 MB/s**：`gamedata` 495 769 600 B / 131 s，`conf` 161 228 800 B / 19 s。
- **核对用 `tar -cf - <dir> | wc -c` 复算**（掌机侧）—— 与 curl 的 `size_upload` 逐字节相等，
  比逐个 `du`/sha 快且不受 vfat 簇开销干扰（vfat 下 `du` 会虚高十几 MB）。
- 抽样 sha256（`libunity` / `libil2cpp` / `libAkSoundEngine` / `globalgamemanagers` / `bundle1`）
  与容器**逐位一致**。文件数 `gamedata` 1167 / `conf` 183。

### C. 启动方式（真机 framebuffer 约束）

`dmenu.bin` 持有 framebuffer，**不能用 `dropbeak exec` 直接当前台跑**（抢显示必输）。
前端给的交接契约是：把命令行写进 `/tmp/.next`，再 `killall -s SIGUSR1 dmenu.bin`，
前端会拆掉自己的 UI 并 `exec` 那一行。⇒ 新增 `Oddmar/relaunch.sh`（照 FiveHearts 的做）：

```sh
sh /mnt/mmc/Roms/ports/Oddmar/relaunch.sh          # 远程拉起
BD_ENV="…" sh /mnt/mmc/Roms/ports/Oddmar/relaunch.sh   # 透传 loader env
```

正常玩还是**从掌机 Ports 菜单点 `Oddmar`**；`relaunch.sh` 是给远程验收用的。

### D. 首跑验收（`SECS` ≈ 75 s，开场过场阶段）

| 判据 | 容器 | **真机** |
|---|---|---|
| `ZZZSENTINEL` / `Unable to read header` / `could not translate` | 0 | **0** |
| `CRASH` / `SEGV` / `terminate` / `Unknown symbol` / `GL error` / `Invalid Reference` / `NRE` | 0 | **0** |
| `[BD-VIDEO] redirect external bind`（画面判据） | 3 | **3** ✅ |
| `Silent mode` | 0 | **0** ✅ |
| `Enqueue` | 217 | **154** ✅ |
| `reusing shared SDL dev` | 1 | **1** ✅ |
| `Sound engine initialized successfully.` | 1 | **1** ✅ |
| GL | llvmpipe（软渲染） | **OpenGL ES 3.2（Mali 真驱动）** |
| warning / retry / missing / not found / failed | 0 | **全部 0** |
| 画面 | 标题界面 | **开场过场动画**（`W1L1start`，维京人抓鸡） |
| 日志 | 187 921 行 | 80 646 行（`LOG=ON` 构建） |

画面取自 `/dev/fb0` 直读（`Oddmar/fbcap.sh`，`dd bs=2457600 count=1`），
本机用 PIL 按 `640×960 BGRA` 解，取结构更丰富的那半帧。

音频链路在真机上与设计**逐字一致**：

```
[BD-OPENSLES] slCreateEngine -> engine 0x…
[BD-OPENSLES] reusing shared SDL dev 2 (24000 Hz x 2 ch x 16 bit); Wwise wants 48000 Hz … -> resampling
[BD-OPENSLES] CreateAudioPlayer: 48000 Hz x 2 ch x 16 bit -> SDL dev 2 (queued-depth target 19200 B)
[BD-OPENSLES] Enqueue #250: 256 B (queued 15616 B)
```

### E. 🔴 真机内存 —— 修正 §C 的推算

| 时刻 | RSS |
|---|---|
| 启动后 ~50 s（过场动画中） | **341 MB**（`hwm` 343 MB） |
| 运行 2.5 min（过场推进中） | **465 MB**（`top RES` 476 MB，`%MEM` 47.8%） |
| 系统 `MemAvailable` | 533 → 412 MB（全程无 swap 抖动） |

- §C 推算的「掌机等效 ≈ 408 MB」**方向对、偏保守**：真机开启阶段只要 **341 MB**，
  比容器 666 MB 少 **325 MB**。差额 = qemu 翻译缓存 128 MB + llvmpipe 47 MB（真机都不存在）
  ＋ glibc / SDL / Mali 驱动差异。
- **§0.10 那 4 个 env 在真机照常生效**；`LP_NUM_THREADS=2` 因无 llvmpipe 而无副作用，
  留着不影响真机。
- 顺带解决 §0.10-G1（真机复测）。**但 §0.10-G2 更尖锐了：这台只有 996 MB 且无 swap。**

### F. 真机才暴露的发现

1. `[BD-VIDEO] source 854x480 is larger than drawable 640x480 — convert+upload pay full cost;
   shrink the MP4 offline before the APK build`
   ⇒ 瘦身时选的 **854×480 对 640×480 面板仍是超采样**。`_slim/video_640x360/`（100 MB）
   是更贴合的档位：再省 32 MB，且解码/上传量落回面板内。**待做（未做）**。
2. CPU 侧有余量：`unityloader` 占 ~1.2 个核（4 核机），系统 `idle 67%`。

### G. 未闭环

1. **进关卡后的内存峰值** + SoundBank（147 MB）死重验证（§0.10-G2 的真机版）。
2. 视频降档到 `640×360`（§F1）。
3. **手柄映射未在真机实测**：`gamecontrollerdb.txt` 已放（从 FiveHearts 复制），
   toml 里 `[input] controller_name="Microsoft X-Box 360 pad"` —— 需实机按键验证。
4. `staging/Oddmar/gamedata` 与掌机不一致（原始版 vs 瘦身版），要么同步要么明确标注。

## 0.12 音频混音器：视频音轨无声 + 音量键无效（2026-09-23 第四轮）

真机反馈两条：**① 开场视频没声音；② 视频结束后界面音乐极大，掌机音量降到 0 也不变。**
结论：两条是同一个架构缺陷的两个侧面 —— **两个音频引擎在抢一个 SDL 设备，旧实现让"抢到的人独占、没抢到的完全静音"**。

### A. 真机取证（只读，未启动游戏）

```
/sys/class/power_supply/axp2202-battery/openbor_volume   = 0        ← 音量键写的就是它
amixer -c 0 sget 'digital volume'                        = 63/63    ← 100%
amixer -c 0 sget 'lineout volume'                        = 31/31    ← 100%
```

`grep -rl openbor_volume /mnt/vendor/bin` ⇒ `dmenu.bin` / `portsCtrl.dge` / 各模拟器 `.dge` 全部引用。
⇒ **这台固件的音量模型是"硬件保持满量程，由应用自己读 `openbor_volume` 缩放 PCM"。**
硬件 DA 一点没动，所以任何"不读这个值"的程序都是满音量、且按键无效。

### B. 日志取证（`log-seqs`，容器两轮）

时间线对得上用户描述的两个阶段（`peak(1s)` = 每秒窗口峰值，本轮新增的指标）：

| 日志行 | 阶段 | FMOD (Unity) | Wwise |
|---|---|---|---|
| 36282 起 | **视频播放中**（`game-start.m4v`） | peak 0 → **20103** | peak **0** |
| 175591 起 | **视频结束、界面** | peak **0** | peak 0 → **17107** |

⇒ **视频音轨走 Unity 音频（FMOD 路径），界面音乐走 Wwise**，两者分别独立。

### C. 根因（两条，都在 port 侧）

**① 旧 `fakefmod.cpp` 在 Wwise 抢占 writer slot 后 `continue`，永不调用 `fmodProcess()`：**

```cpp
if (bd_audio_bus_owner() != BD_AUDIO_OWNER_FMOD) { sleep(5ms); continue; }   // ← 旧代码
```

`fmodProcess()` 是 **Unity 整个音频系统唯一的 pull 接口**，停掉它 = Unity 混音器完全静止。
而 `VideoPlayer.audioOutputMode` 默认是 `AudioSource`，视频 AAC 走的就是这条 → **视频没声音**。

排除法佐证（不是猜测）：
- 日志中 `android/media/AudioTrack` 出现 **0 次**（Direct 模式需要它）；
- `libunity.so` 的 dynsym 里无任何 `AudioTrack` / `libmedia` / `AAudio` UND 符号。
  ⇒ 视频音频只能是 `AudioSource` 路径，即 FMOD。

**② `opensles.cpp::BufQ_Enqueue` 把 Wwise 的 PCM 直推 `SDL_QueueAudio`，零增益处理。**
`bd_sys_volume_percent()` 只在 `fakefmod` 里被读（而那条路已被①静默）⇒
**Wwise 音乐永远满量程，音量键对它零作用** ⇒ "降到 0 也不变"。

### D. 修法：把两个引擎混到一条出口

`SDL_QueueAudio` 是纯 FIFO，两路直接推会交错成噪声 —— 这正是旧实现选择"独占"的原因。
正确做法是**在 FIFO 之前做软件混音**，于是 `platform/common/audio_bus.{h,cpp}` 从"设备登记处"
升级成真正的 mixer：

```
bd_audio_bus_push(FMOD|WWISE, pcm, samples)   producer：各自写自己的 ring
bd_audio_bus_pump(dev)                        consumer：逐样本相加 + 饱和 + 统一增益 + 补 SDL 队列
```

- **队列水位 75 ms**（`BD_AUDIO_QUEUE_MS` 可调）。之前 FMOD 用 4096 B（42 ms）、Wwise 用 100 ms。
- **增益在 pump 里统一施加**（混合之后），所以两个引擎一起受控；`bd_sys_volume_poll()` 每 ~80 ms
  读一次后端（sysfs 文件），不必每次回调都读盘。
- **ring 有上限**（各 1 s），生产者落后时丢最老的，不会把播放越推越晚。
- **pump 由两个生产者线程各自调用**（FMOD 5 ms 循环 / Wwise refill 线程 5 ms），锁保护，谁先到谁干活。
- Wwise 的 refill 判据从"SDL 队列深度"改为"自己 ring 的深度"（SDL 队列已归 mixer 管），
  且**单次 tick 内循环 refill**（一次回调只有 2.7 ms 音频，否则喂不饱）。

### E. 顺带修掉的两处设备生命周期 bug（本轮没触发，属防御）

借来的设备是 **FMOD 的**，混音器正在用它播视频音轨：

- `Play_SetPlayState` 的非 PLAYING 分支原本无条件 `SDL_PauseAudioDevice(p->dev, 1)`；
- `Object_Destroy` 原本无条件 `SDL_PauseAudioDevice + SDL_CloseAudioDevice`。

Wwise 一旦回收 player，就会**把整个出口关掉**（连视频音轨一起）。现以 `PlayerState::owns_device`
区分"自己开的"与"借来的"，只有自己开的才允许暂停/关闭。

### F. 验收（容器，`build-oddmar-verbose`，两轮）

判活：`redirect external bind`=**3**、`Silent mode`=**0**、`Sound engine initialized`=**1**、
`GL error`=0、无 CRASH/SEGV、截图有内容（mean 16801–37869，非 192 B 黑帧）。

```
[BD-AUDIO] bus published dev=2 24000 Hz x 2 ch x 16 bit, queue target 7200 B
[BD-AUDIO] mix 6772800 B | fmod pushed=3377152 smp peak(1s)=0     (q=2048 drop=0) |
                          wwise pushed=3246336 smp peak(1s)=11759 (q=4848 drop=0) |
                          queued=7200/7200 B | gain=50%
```

- 两路 `pushed` 都以实时速率增长，且 `g_mixed_bytes` 持续增长 ⇒ **设备确实在消费**（队列没卡住）。
- `peak(1s)` 精确复现用户描述的两阶段（§B 表）。
- 第二轮用 `BD_SYS_VOLUME_BACKEND=sysfs BD_SYS_VOLUME_PATH=/tmp/fakevol`（值 5/10）验证音量链路：
  `volume backend=sysfs ... percent=50 keys=1` → **`gain=50%`** ⇒ 缩放确实落到输出上。✓

⏳ **上机待验**：容器无声卡，只能证明"数据流 + 增益"成立；**真实听感必须上机**（§0.9 同款限制）。

### G. 上机（已完成部分）

新版 `build-oddmar`（Release + LOG=ON + strip，6 656 000 B，sha256 `f9d3f484…498f7c`）已推到
`/mnt/mmc/Roms/ports/Oddmar/unityloader`，旧版备份为 `unityloader.bak-preaudio`。
**未在掌机上启动游戏**（按用户要求）。

### H. 未闭环 / 风险

1. **两路同时有强信号时会削波**：mixer 只在相加后做饱和（不做 headroom 缩放）。
   实测两路各自 peak 约 20 k / 17 k，若同一时刻都在响就会触到 32767。上机听感确认。
2. **音量键在游戏运行时能否被 SDL 收到**：`input_backend.cpp` 拦的是
   `SDL_SCANCODE_VOLUMEUP/DOWN`。掌机音量键若不经过 Linux input 层（PMIC 直连），
   port 与 dmenu 都收不到 ⇒ 需要另找路径。**上机必须实测。**
   ⚠️ **上机前先把 `openbor_volume` 设成非 0**：修复后 mixer 会认真对待这个值，
   原先留着的 `0` 会让游戏**完全静音**（已从 0 改为 **7**，/10）。
   若音量键确实收不到，会卡在"改不了音量"的死角 —— 这是本轮最大的上机风险。
3. 视频音画同步的实测延迟 = FMOD ring(≤21 ms) + SDL 队列(75 ms) ≈ **~96 ms**，
   可用 `BD_AUDIO_QUEUE_MS` 收窄。上机看是否可察觉。

## 0.13 按键：ABXY 变"返回" / 关卡弹"是否退出游戏？" —— 为什么只有 Oddmar（2026-09-23 第五、六轮）

> **2026-09-23 晚间更正**：本节 §0.13～§0.13c 保留排查史，但最终判定以 **§0.13d** 为准。
> 最新真机证据显示：`guide/select/back = NONE` 与 `Start+Select` 长按都不是 ABXY 弹退出框的根因；
> 用户要求已撤销这两项改动。ABXY 弹框由 Oddmar/InControl 的 Android 手柄按钮路径触发，
> 当前有效方向是将 Oddmar 的面键映射到该游戏 legacy InputManager 的键盘语义（A/Y=SPACE，B/X=K）。

**用户现象**：同一台掌机上 Samurai2 / Maximus2 按键完全正确；Oddmar 里按 guide / select / back 会弹
"是否退出游戏？"，严重时感觉"所有键都变成返回"。

### 结论先行：**Oddmar 的 InControl 把 `KeyCode.Escape`、`KeyCode.ButtonMode`、`KeyCode.ButtonSelect` 都当 `Back` 用**

**这不是映射错位。** loader 送出的 keycode 逐键正确；问题在于 Oddmar 用的是 **InControl** 输入框架，
其 Android/Xbox profile 把 `InputControlType.Back` 绑在 `KeyCode.Escape` 上，并且一旦进入关卡加载/关卡地图，
`KeyCode.ButtonMode`(110) 和 `KeyCode.ButtonSelect`(109) 也会被它当成返回语义 ⇒ 弹「是否退出游戏？」。
标题界面和未进关卡时这些键不一定触发，所以用户最初把问题描述成"ABXY 也变成返回"。
展开见下方「为什么只有 Oddmar」。

用**掌机真机探针**（`/mnt/mmc/padtest/sdlpad`，同源系统 SDL，同时读 `/dev/input/event1` 原始 `EV_KEY`
与 SDL `CONTROLLERBUTTONDOWN`）拿到"内核码 ↔ SDL 索引"配对，用户实按三键全部精确对上：

| 用户按的键 | 内核 keycode | SDL 游戏手柄索引 | 内置表项 | 注入的 Android keycode |
|---|---|---|---|---|
| **A** | 304 | **0** | `a:b0` | **96** `KEYCODE_BUTTON_A` ✓ |
| **Y** | 306 | **2** | `y:b2` | **100** `KEYCODE_BUTTON_Y` ✓ |
| **X** | 307 | **3** | `x:b3` | **99** `KEYCODE_BUTTON_X` ✓ |
| B（按排除法） | 305 | 1 | `b:b1` | 97 `KEYCODE_BUTTON_B` ✓ |
| D-pad | `ABS_HAT0X/Y` | 11–14 | `dp*:h0.x` | 19–22 ✓ |
| 音量键 | 114/115 | — | 未映射 | **不产生任何手柄事件**（不抢键）✓ |

⇒ SDL 2.0.12 的**两段式编号**（先 `BTN_JOYSTICK(288)..KEY_MAX`，再 `0..287`）得到实测确认
（304→0、306→2、307→3 严格递增）；**内置映射表就是为这台设备写的，逐键正确**。
`[BD-INPUT-REMAP] a -> BUTTON_A (keycode 96)` … 也逐条正确。

### 排掉的四个"看似变量"

| 对照项 | 证据 | 判据 |
|---|---|---|
| `[input]` + `[input.remap]` | 各游戏 `unity.toml` 逐字比 | **逐字相同**（含 `guide = "ESCAPE"`） |
| loader 内置手柄映射表 | 7 个 loader 的 `ANBERNIC-keys,…` 串 | **逐字节相同** |
| SDL 库 | `ldd` | 都链**系统** `libSDL2-2.0.so.0` ⇒ 同一份 SDL、同一套编号 |
| 映射库文件 | `md5sum gamecontrollerdb.txt` | Oddmar 与 FiveHearts **相同** |
| loader 源码版本 | `git log -S` | `load_input_remap`(2026-05-15 `70f683b`)、A↔B swap(`1515885`/093c569, 2025-09) **都早于包内 loader** ⇒ 输入代码同源 |

⚠️ `strings` 在包内 loader 里查不到 `INPUT-REMAP` **不代表功能缺失** —— 包内是 `LOG=OFF`，
`BD_LOG` 的格式串会被编掉；只有非日志串（如 `dpad_synthesize_hat`）才靠得住。

### 为什么只有 Oddmar：它跑的是 **InControl**

**证据 1 —— 真机日志里 ESCAPE 只来自 guide**（同一轮 20.4 万行日志，`[BD-PAD]` 共 83 行）：

| 物理键 | SDL 逻辑按钮 | 注入的 Android keycode |
|---|---|---|
| A / B / X / Y | 0 / 1 / 2 / 3 | **96 / 97 / 99 / 100**（`BUTTON_A/B/X/Y`）✓ |
| Start / Select | 6 / 4 | **108 / 109**（`BUTTON_START/SELECT`）✓ |
| D-pad | 11–14 | **19–22**（`DPAD_*`）✓ |
| **guide** | **5** | **111 `ESCAPE`** ← 全日志唯一来源（两次按下，各 2 行） |

**证据 2 —— 时间对齐**：`188437 [BD-PAD] DOWN btn=5 (guide) kc=111` 之后 20 行就是
`188457 [BD-ANDROID] Unity: Fade Out`；同一次运行里游戏侧 `Fade Out` **只出现这一次**，
而 A/B/X/Y/Start/Select 那些按键前后**没有任何游戏侧反应** ⇒ 弹窗只由 guide 唤起。
⇒ 用户"ABXY/Start/Select 都会弹"的印象，来自"弹窗起来之后，任意键都在和弹窗交互"
（他随后按 B 关掉弹窗并跳跃，正是这个模式）。

**证据 3 —— 游戏侧是什么在吃 ESC**：`strings global-metadata.dat` 里有整片
`InControl.UnityDeviceProfiles.*`（含 **`Xbox360AndroidUnityProfile`**），游戏日志第 1159 行自报
`InControl (version 1.8.6 build 9370)`。InControl 的 Android/Xbox profile 把
**`InputControlType.Back` → `KeyCode.Escape`**（Android 上系统 Back 键就是 ESC）。
ESC 进 Unity 就是 `KeyCode.Escape` = 框架级 Back ⇒ 弹「是否退出游戏？」。

**这也解释了"同一台掌机、同配置，别的游戏不犯病"**：Samurai2 / Maximus2 直接用 Unity 原生 Input，
Escape 对它们只是普通键、没绑行为；**只有 Oddmar 套了 InControl 这层抽象，把 Escape 当 Back**。

顺带核对了 Unity 的 Android→JoystickButton 映射：A(96)→JB0、B(97)→JB1、X(99)→JB2、Y(100)→JB3、
L1(102)→JB4、R1(103)→JB5、START(108)→JB10、SELECT(109)→JB11 —— 与 InControl Android profile 的
（Action1..4 = Button0..3、LeftBumper=4、RightBumper=5、Start=Button10、Select=Button11）**逐项吻合**，
所以"Start 反应是对的"这一观察也自洽。

### 修法（第六轮落）

| 改动 | 位置 |
|---|---|
| `guide = "ESCAPE"` → **`guide = "NONE"`**；新增 `select = "NONE"`、`back = "NONE"` | 掌机 `unity.toml` + staging `Oddmar/unity.toml` + `_sync/container-unity.toml` |
| `parse_keycode_name()` 补全 `BUTTON_C` / `BUTTON_Z` / `BUTTON_MODE` / `BUTTON_L3` / `BUTTON_R3`；<br>构造 `KeyEvent` 前对 `KEYCODE_UNKNOWN` 显式跳过，让 `NONE`/`DISABLE` 真能不注入事件 | `platform/common/input_backend.cpp` |
| `Start+Select` 退出热键改成长按 **1200 ms** | `platform/common/input_backend.cpp` + `unity.toml` |

⚠️ 掌机 Select 键在 SDL 游戏手柄表里的名字是 `back`（`ANBERNIC-keys,…,back:b6,…`），所以只改 `select` 不够，
必须同时改 `back`，否则 `back` 会回落到 `toAndroidKeycode()` 的 default → `KEYCODE_BUTTON_SELECT(109)`，照样弹窗。
⚠️ 即使把 `guide` 从 `ESCAPE` 改成 `BUTTON_MODE`(110)，在关卡加载/关卡地图仍会被 InControl 吃掉并弹同一个框；
**只有 `NONE` 才能彻底屏蔽**。
⚠️ **`guide = "ESCAPE"` 是全部 port 的模板默认值**（5 个 stage + 4 个 configs 都有）。本轮只改 Oddmar：
别的游戏实测无害；但**任何套了输入抽象层（InControl / Rewired）或自己把 Escape / ButtonMode / ButtonSelect 当返回的游戏都必须改**。

**验收判据**：日志出现 `[BD-INPUT-REMAP] guide -> NONE (keycode 0)`、`select -> NONE (keycode 0)`、`back -> NONE (keycode 0)`；
按 guide/select/back 时**没有**对应的 `[BD-PAD] DOWN`；标题界面与关卡地图不再弹"是否退出游戏？"。

**容器回归（2026-09-23，按键回放，75 s / 80 s）**：活满、`BD-EXIT`=0、`redirect external bind`=3、
`Silent mode`=0、`GL error`=0、无 CRASH/SEGV。**4 个 `guide=NONE` 轮次全程无退出对话框**；
按 A 进入关卡加载后也不再弹（早前"仍弹"的记录已推翻，理由与截图字节数判据见 §0.13c）。
掌机侧已推：`unityloader` sha256 `4d30e5b6…`（6 651 904 B，备份 `unityloader.bak-preholdfix`）、
`unity.toml`（备份 `unity.toml.bak-preholdfix`）。

### 附带事实（原假设已修正，别再走回头路）

- **不是** `controller_name` 拼写问题，**不是** SDL 编号错位，**不是** loader 版本差异（见上表对照）。
- Oddmar 的 InputManager（`gamedata/assets/bin/Data/globalgamemanagers`）**确实只有键盘绑定**
  ——`Jump = space`、`Submit = return/enter`、`Cancel = escape`、`Action1 = q`、`Action2 = e`、
  `Fire1 = k`、`Fire2 = l`、`Horizontal = a/d+left/right`、`Vertical = s/w+down/up`、`Horizontal2 = q/e`
  —— **一条 `joystick button N` 都没有**。但**这不代表手柄会被送进键盘路径**：InControl 自己通过
  `Input.GetJoystickNames()` 建设备与 profile，与 legacy InputManager 的轴表无关。
  ⇒ 原文档据此推出的"把手柄映射成 SPACE/ESCAPE/Q/E 的键盘化方案"是**错方向，已废弃**。
- 游戏自报 `controllerType: GameController`（而非 `Xbox`）只影响按键图标/提示，与本次故障无关。
- 掌机这个 Menu/Guide 键的内核码是 **`BTN_TL2`(312)**，同一次按下还会多报一个 **`KEY_GOTO`(354)**
  （固件的"菜单/返回前端"语义）；后者落在 SDL 里**无绑定**的 raw joy button 11 上，
  不产生任何手柄事件，无副作用（探针实测：`CBUTTON button=5 name=guide` + `JBUTTON index=8/11`）。

### 可复用手法：按键"变味"必须分三跳查

| 跳 | 取证手段 |
|---|---|
| ① 内核 evdev 码 | 掌机探针 `./sdlpad <秒> <输出>`：同时读 `/dev/input/event1` 原始 `EV_KEY` 与 SDL 事件 |
| ② SDL 逻辑按钮 | loader 启动日志 `[BD-PAD-MAP]`（`GetBindForButton` dump + remap 表 + raw 按钮数） |
| ③ 注入的 Android keycode | 每次按键一行 `[BD-PAD] DOWN btn=… (名字) kc=… remapped=yes/no` |

⚠️ **只查第 ③ 跳的配置往往不够** —— 必须再问一句"**这个游戏用什么输入框架**"：
`strings global-metadata.dat \| grep -E "^InControl\.|^Rewired"`，框架会在游戏日志里自报版本。
⚠️ 对齐"按键 → 游戏反应"最省力的锚点，是游戏侧 `[BD-ANDROID] Unity:` 日志的**行号**（本轮靠 `Fade Out` 一击命中）。

### 本轮新增的可复用资产

- 掌机按键探针：`.workbuddy/tmp/sdlpad.c` → 容器交叉编译 → `/mnt/mmc/padtest/sdlpad`
  （用法 `./sdlpad <秒> <输出文件>`；`--fg` 前台直出，便于短测）。`EVIOCGRAB rc=0` ⇒ 前端没独占输入设备。
- Dropbeak 四个坑：覆盖**正在执行**的二进制必失败（先 `killall` 再 `push`）；`exec` 客户端 **20–30 s 断连**
  （长任务要 `setsid … <out>` 落文件、事后另开短命令读）；**可写根不含 `/tmp`**（用 `/mnt/mmc/**`）；
  嵌套引号会破坏远端命令 ⇒ **写成 `.sh` 推上去执行**。
- 老基线日志存档：掌机 `log-round-1049-audio-input.txt` + 本地 `.workbuddy/tmp/padlog/`（34 MB）。

### 0.13b 补充（第六轮）：两次真机测试都是**被 loader 的 Start+Select 热键杀掉的**

用户反馈"问题依旧"。**先看日志末尾 —— 两份真机日志的最后三行完全一样**：

```
[BD-EXIT] Start+Select exit hotkey (controller)
[BD-PREFS] saved 'com.mobge.Oddmar.v2.playerprefs' (8 records)
[...] unityloader exited (0)
```

| 轮次 | 日志 | 行数 | 终局 | 死前最后两条按键 |
|---|---|---|---|---|
| 10:49 那轮 | `log-round-1049-audio-input.txt` | 236 948 | `[BD-EXIT] Start+Select` | — |
| 12:45 那轮 | `log.txt` | 135 691 | `[BD-EXIT] Start+Select` | `DOWN btn=6 (start) kc=108` → **125 行后** `DOWN btn=4 (back) kc=109` |

**⇒ 用户两次"测试"都没有跑完：第二次在按下 Start 后的**几十毫秒内**又按了 Select，
`bd_exit_hotkey_update()` 立刻判定"两键同按"→ `bd_flush_prefs_impl(); _exit(0);`，
游戏被**当场杀死**，日志里连 `UP` 都来不及记录。用户看到的是"游戏又出问题了"。**

⚠️ **掌机上 Start 与 Select 相邻**，而"先按 Start 开菜单、再按 Select 退出/返回"是玩家的自然动作
⇒ **这个热键在掌机上的误触率极高**。旧行为（任一键 DOWN 时若另一键仍处于 down 即退出）等于**没有防抖**。

**修法**：`bd_exit_hotkey_update()` 拆成"记账 + tick 超时"两段 —— 加上 `_update()` 只记按下时刻并打日志，
新增 `bd_exit_hotkey_tick()` 挂在事件循环（`SDL_Delay(4)` 之后，因为按住不动时 `SDL_PollEvent` 一个事件都不返回，
纯事件驱动等不到超时），**两键同按满 `start_select_exit_hold_ms`（默认 1200 ms）才退出，松开任一键即取消**。

| 改动 | 位置 |
|---|---|
| `bd_exit_hotkey_fire/update/tick` 三段式 + `input_exit_hold_ms`（`start_select_exit_hold_ms`，默认 1200，`0`=旧行为，`start_select_exit=false`=整个热键关闭） | `platform/common/input_backend.cpp` |
| `start_select_exit = true` + `start_select_exit_hold_ms = 1200` | staging `Oddmar/unity.toml` + `_sync/container-unity.toml` |

**验收判据**：启动日志出现 `[BD-INPUT-REMAP] start_select_exit = on, hold 1200 ms (0 = instant)`；
按下 Start+Select 出现 `[BD-EXIT] Start+Select down (controller) — hold 1200 ms to exit, release to cancel`，
**松开则 `… released before 1200 ms — exit cancelled`**；只有真按住 1.2 s 才出现 `Start+Select exit hotkey (controller, held 1200 ms)`。

⚠️ **通用教训**：`[BD-EXIT]` / `[BD-PREFS] saved` / `unityloader exited (0)` 这三行连着出现
= **进程不是崩的，是被 loader 自己请出去的**。排查任何"游戏突然没了"，先看这三行；
别把它当成游戏崩溃去查 SEGV。**任何时候看到用户报"问题依旧"，第一件事是把日志末尾读完。**

### 0.13c 另一路触发源：`back`/`select` 与 loader 的 Start+Select 热键（第六轮）

| 现象 | 根因 | 修法 |
|---|---|---|
| 真机两次运行都在几十秒后**进程消失** | loader **自己的** `Start+Select` 退出热键被误触（掌机上两键相邻，"先 Start 开菜单再 Select 返回"是自然动作），`_exit(0)` 直接杀进程 | 改成长按 **1200 ms** 才生效（`start_select_exit_hold_ms`），松开任一即取消 |
| 关卡加载 / 关卡地图仍弹「是否退出游戏？」 | `back`（SDL 层名字，对应掌机 Select）默认 → `KEYCODE_BUTTON_SELECT`(109)，同样被 InControl 当 Back 用；`guide` → `BUTTON_MODE`(110) 也一样 | `[input.remap]` 里 `guide` / `select` / `back` **三个键全部设 `NONE`**（`parse_keycode_name()` 把 `NONE`/`DISABLE`/`OFF` 解析成 `KEYCODE_UNKNOWN`，该键不再向游戏注入任何 KeyEvent） |

⚠️ **`select` 和 `back` 是同一个物理键的两个 SDL 名字**，只写 `select = "NONE"` 无效——必须两个都写。
（`toAndroidKeycode()` 里 `SDL_CONTROLLER_BUTTON_BACK → KEYCODE_BUTTON_SELECT`，所以只禁 `select` 会以为已经关掉了。）
⚠️ A/B/X/Y 保持正常映射。注意 `toAndroidKeycode()` 里 **A↔B、X↔Y 是故意对调的**（掌机确认/取消习惯），
`[input.remap]` 的显式绑定会覆盖它。

**容器按键回放证据（`BD_PAD_REPLAY`，`build-oddmar`）**：4 个 `guide="NONE"` 且回放覆盖
A/B/X/Y/Start/back/guide 的轮次（`seq-nn` / `seq-la` / `seq-bn` / `seq-cn`）**全程没有出现退出对话框**；
唯一出现对话框的 `seq-full` 用的是 `guide="BUTTON_MODE"`。**用截图字节数就能判别画面阶段**：
对话框轮次会掉到 **26–27 KB 的暗屏**，正常关卡地图约 **229 KB**，进入关卡后约 **100 KB**，
中段恒定的 **33–34.5 KB** 是过场黑屏。
⚠️ **早前凭肉眼看图得出的"A 进入关卡也触发弹窗"结论已推翻**——`seq-la`（只按 A）与全键轮的帧尺寸序列一致，
说明那 100 KB 是正常关卡画面而不是弹窗。**别再用"肉眼读 PNG"当判据，用字节数序列。**

**容器回归（75 s / 80 s，`build-oddmar`）**：活满、`BD-EXIT`=0、`redirect external bind`=3、`Silent mode`=0、
`GL error`=0、无 CRASH/SEGV。长按版另测：`10000:start,10080:back`（重叠 80 ms）**不退出**；
`BD_PAD_REPLAY_HOLD=2500` 时 `unityloader exit=0 after 12047ms` 且日志 `held 1200 ms` ⇒ 长按语义成立。

掌机已推：`unityloader` sha256 `4d30e5b6…`（含长按修复 + 回放探针，env 未设时不生效）、`unity.toml`（`guide`/`select`/`back` = `NONE`；
备份 `unityloader.bak-preholdfix` / `unity.toml.bak-preholdfix`）。**真机复测待做。**

### 0.13d 晚间更正：ABXY 弹框不是 guide/select/back；是 Oddmar 的 Android 手柄按钮路径

**最新结论（以此为准）**：

- 用户要求撤销两项旧修法：`Start+Select` 长按 1200 ms、`guide/select/back = NONE`。
  当前掌机恢复为即时 Start+Select 热键，`guide = "ESCAPE"`、`select/back = "BUTTON_SELECT"`。
- “Press any button” 界面按 A/B/X/Y 弹「是否退出游戏？」不是 SDL 编号错位，也不是 ESC/Back 键混入；
  日志里 ABXY 均正确注入为 Android 手柄按钮码（A=96、B=97、X=99、Y=100），无 ESC/Back。
- 关键 A/B 真机对照：
  - `a = "BUTTON_A"`：第一次 A 弹退出确认框；第二次 A 对默认 Yes 执行 `UnityPlayerActivity.finish()`；
  - `a = "SPACE"`：Press-any-key 不弹框；再按 A 可进关；关卡内 A 只跳跃。
  因此问题在 Oddmar/InControl 对 **Android 手柄按钮路径** 的处理，不在 loader 的普通 keycode 映射。
- B/X/Y 同理：改成键盘语义后，Press-any-key 界面分别按 A/B/X/Y 都不再弹退出框。

**当前掌机配置（实验态）**：

```toml
[input.remap]
start  = "BUTTON_START"
select = "BUTTON_SELECT"
back   = "BUTTON_SELECT"
a      = "SPACE"   # Jump / UI confirm
b      = "K"       # Attack / UI confirm
x      = "K"       # Attack duplicate
y      = "SPACE"   # Jump duplicate
guide  = "ESCAPE"
```

真机表现：

| 场景 | A | B | X | Y |
|---|---|---|---|---|
| Press any button | 不弹退出框 | 不弹退出框 | 不弹退出框 | 不弹退出框 |
| 主界面 / Start 菜单 | 确认 | 确认 | 预期确认（K） | 预期确认（SPACE） |
| 关卡中 | 跳跃 | 攻击 | 攻击副本 | 跳跃副本 |

**未闭环问题（当前最大残留）**：

- 键盘化后首次动作正确，但用户实测“同一键第二次不触发”；A/B 交替也不能恢复；
  只有先按一次 D-pad，再按 A/B 才会再次动作。
- 日志确认新版已把 A 发成 keyboard device/source（`device=1 source=0x101 keycode=62`），且 DOWN/UP 都进入 Unity；
  所以这不是“UP 没发出”或“仍挂在手柄设备”。
- 当前已部署一个实验版：键盘化面键 `ACTION_UP` 后补发一帧中立手柄 `MotionEvent`
  （模拟手动按 D-pad 触发的输入刷新），loader sha256：
  `d0aaa1de1d989a2524cec1102850574acf25233886c77b8b8167dafb477b02bd`。
  **该实验尚待用户真机反馈**，不能写成已修复。

**本轮保留的 loader 侧工具/诊断**：

- `[BD-PAD] DOWN/UP btn=... kc=...`：逐键确认 SDL button → Android keycode；
- `[BD-PAD-MAP]`：启动时 dump SDL 映射、raw button 数、remap 表；
- `BD_PAD_REPLAY`：容器内自动回放按键；
- `nativeInjectEvent keycode/action/device/result`：确认 Unity native 注入成功；
- 默认屏蔽高频 `[BD-FINDCLASS]` 与 `[BD-ANY-MISS]`，需要完整 JNI 噪声时用 `BD_JNI_TRACE=1`。

**下一步建议**：

1. 先让用户验证 sha `d0aaa1de…` 的“中立 MotionEvent 刷新”是否解决连续 A/B。
2. 若仍无效，继续沿“Unity legacy keyboard state 没刷新”查，而不要回到 guide/select/back：
   - 尝试让键盘化面键走真实 `SDL_KEYDOWN/UP` 分支等价路径；
   - 或在 remap 层为键盘化按钮合成 `MotionEvent + KeyEvent` 的不同顺序/延迟；
   - 必要时反查 Unity 2018 Android legacy keyboard 状态是否只在特定 input update 阶段刷新。

## 1. 环境与复现


### 1.1 容器与路径

- 容器 `GlES_Dev`（`dropbeak-gles-dev:local`，aarch64 Ubuntu 20.04 + g++ 9.4 + Xvfb + Mesa/llvmpipe）
- 仓库在容器内：`/workspace/Bogodroid`
- 游戏根：`/game/Oddmar`（`gamedata/` 为资源，`unity.toml` 为配置）
- 工作副本（重要）：`/tmp/bd-unityloader`、`/tmp/bd-oddmar.toml`

### 1.2 构建

```bash
# 主机 → 容器同步（每个改动文件都要单独 cp）
docker cp "<本地路径>" GlES_Dev:/workspace/Bogodroid/<相对路径>

# 增量构建（debug 版，RelWithDebInfo，带 -g、BD_ENABLE_LOG/VERBOSE 已开）
docker exec GlES_Dev bash -c 'cd /workspace/Bogodroid && \
  cmake --build build-oddmar-verbose -j16 --target unityloader 2>&1 | tail -5; \
  echo "BUILD_EXIT=${PIPESTATUS[0]}"'
```

- `build-oddmar-verbose/` = 诊断构建目录（`CMAKE_BUILD_TYPE=RelWithDebInfo`，带 `-g`，LOG/TRACE/VERBOSE 全开，≈150 MB）
- `build-oddmar/` = **上机中间态**（`Release` + `BD_ENABLE_LOG=ON` + TRACE/VERBOSE OFF + strip，≈6.6 MB）
  —— `/run-oddmar-seq.sh` 的默认 `BOOT_LOADER`。**每次改动后跑测前确认它的 mtime**（`ls -la`），
  否则你在跑旧二进制。
- ⚠️ **`docker cp` 保留 mtime ⇒ ninja 会说 "no work to do"**：cp 完源码必须 `touch` 一下再构建。
- ⚠️ **`JNIVM_ENABLE_RETURN_NON_ZERO` 是 CMake 缓存项**，共用构建目录时务必确认 `grep JNIVM_ENABLE_RETURN_NON_ZERO build-oddmar-verbose/CMakeCache.txt` 为 `OFF`

### 1.3 运行（逐帧截图 + loader 侧 glReadPixels）

```bash
docker exec GlES_Dev bash -c 'rm -rf /game/Oddmar/seqN /game/Oddmar/log-seq.txt; \
  mkdir -p /game/Oddmar/seqN; cd /game/Oddmar && \
  BOOT_LOADER=/workspace/Bogodroid/build-oddmar-verbose/unityloader \
  BD_BYPASS_VIDEO_TRANSLATE=1 \
  SEQ_DIR=/game/Oddmar/seqN SECS=32 BD_ENABLE_LOG=1 bash /run-oddmar-seq.sh \
  > /game/Oddmar/seqN/run.out 2>&1; echo "EXIT=$?"'
```

> 🔴 **`BD_BYPASS_VIDEO_TRANSLATE=1` 不能漏** —— 见 §1.4-0。漏了就是"全黑 + 看起来像代码坏了"。

产出：
- `/game/Oddmar/log-seq.txt` —— loader 的 stderr（**主日志**）
- `SEQ_DIR/0000.png…` —— X root window 截图（`import -window root`）
- `SEQ_DIR/frame.{1,2,3}.gl.ppm` —— loader 自己 `glReadPixels` 的 drawable（由 `BD_DUMP_FRAME`/`BD_DUMP_FRAME_AT` 触发）

### 1.4 ⚠️ 五个必踩的坑

0. 🔴 **跑测不设 `BD_BYPASS_VIDEO_TRANSLATE=1` ⇒ 必然黑屏。这不是故障，是 opt-in 行为。**
   挡画面的 hook 是 **env 门控、默认不装**（`projects/unityloader/main.cpp::bypass_video_translate`；
   §0.6-H / §0.8 都写明它 opt-in）。漏了它的后果**完整复刻修复前的黑屏**：
   截图恒 192 B（含子窗口单独截图）、`[BD-VIDEO] redirect external bind` = **0**。

   **判据**：跑测日志里 **`redirect external bind` 必须 = 3**；`[BD-VIDEO]` 行数 ~50–70（Release 版）/ 60+。

   ⚠️ 2026-09-22 深夜为此白跑了一整轮排查 —— 现象与"代码改坏了"**完全一样**，
   而且 **A/B 逃逸开关、换显示号、起全新 Xvfb、换旧二进制全都无法区分**，
   因为真正的变量是一个**没 export 的环境变量**。
   **记住这条："换了没变化"不等于"改动无关"。**

1. **`BOOT_LOADER` 默认值是 `build-oddmar`（上机中间态），不是你以为的那个**
   `/run-oddmar-seq.sh` 里 `BOOT_LOADER="${BOOT_LOADER:-/workspace/Bogodroid/build-oddmar/unityloader}"`。
   想跑诊断版要显式传 `BOOT_LOADER=.../build-oddmar-verbose/unityloader`；跑上机态就用默认。
   **改了代码没重新构建时也别指望它生效 —— 见下一条的 mtime 警告。**
2. **bash 环境缺 PATH**
   每条命令前加：
   `export PATH="/usr/bin:/bin:/c/Windows/System32:/c/Users/Administrator/.workbuddy/binaries/PortableGit/versions/1.2.0/usr/bin:$PATH"`
   （否则 `dirname: command not found`，docker 相关命令可能拿不到输出。）
3. **`docker cp` 用绝对 Windows 路径**，且 `javastubs/` 有**两个**目录：
   - 仓库根 `javastubs/`（`android*.cpp`、`javac.cpp`、`bd_assetlocator.cpp` …）
   - `projects/unityloader/javastubs/`（`unity.cpp`、`fakefmod.cpp`、`binding.cpp` …）
   两边都在 `-I` 里，**别 cp 错目录**。
4. **内存调优的 4 个 env 只能由 shell 设，不能在 loader 里 `setenv()`**（§0.10）
   `MALLOC_ARENA_MAX` / `MALLOC_TRIM_THRESHOLD_` / `MALLOC_MMAP_THRESHOLD_` / `LP_NUM_THREADS` ——
   glibc 在 **`main()` 之前**就把 `MALLOC_*` 读走了，loader 内部再设太晚。
   实测这组省 **~83 MB**（标题界面 666 → 583 MB，−12.5%），无性能退化。
   ⚠️ 判可裁剪性**只看 RSS**，别被 `du`/映射虚拟大小带偏：`libicudata` 文件 27 MB 但 RSS 几乎为 0（§0.10-F）。

---

## 2. 必读：libjnivm 日志过滤器的坑（本次最大的时间黑洞）

`libjnivm/src/jnivm/internal/log.h`：

```cpp
inline bool jnivm_log_enabled(const char* tag, const char* format)
{
    static const bool trace = [] { const char* v = std::getenv("BD_JNI_TRACE");
                                   return v && *v && std::strcmp(v, "0") != 0; }();
    if (trace) return true;
    if (tag && std::strncmp(tag, "BD-", 3) == 0) return true;      // ← 看的是 tag，不是 format
    static const char* diagnostics[] = { "STUB-MISS", "Exception", "Fatal", "failed",
        "Failed", " is null", " unsupported", " unknown", "Unknown",
        "Not Implemented", "Unimplemented" };
    for (const char* m : diagnostics) if (std::strstr(format, m)) return true;
    return false;
}

#define LOG(tag, format, ...) \
    do { if (jnivm_log_enabled(tag, format)) \
        fprintf(stderr, "[" tag "]: " format "\n", ##__VA_ARGS__); } while (0)
```

**后果**：写 `LOG("JNIVM", "[BD-MISS] ...")` 会**被静默丢弃**——因为 tag 是 `"JNIVM"`，不以 `BD-` 开头，而 format 里也没有白名单关键字。
**我自己就因此差点得出完全错误的结论**（"没有 RegisterNatives 调用"），浪费了大量时间。改 tag 为 `BD-REG` / `BD-JNIVM` 后，真相立刻出现。

**规则**：
- 自定义诊断一律 `LOG("BD-XXX", "...")`（tag 以 `BD-` 开头）。
- 需要看全部 jnivm 内部流量：启动时 `BD_JNI_TRACE=1`（**极大量，慎用**）。
- 现存的 `LOG("JNIVM", ...)` 只有 format 命中白名单（如含 `STUB-MISS`）才会输出。

---

## 3. `<init>` 解析机制（理解本次所有修复的前提）

### 3.1 查找侧（`libjnivm/src/jnivm/internal/method.cpp`，`jnivm::GetMethodID`）

原逻辑：

```cpp
if(!isStatic && sname == "<init>") {
    // "(args)V"  →  "(args)L<nativeprefix>;"，然后按 static 再查一次
    ...
    return GetMethodID<true, ReturnNull, AllowNative, trace>(env, cl, str0, ssig.data());
}
else { /* 在 cur->methods 里线性查找，按 find_if 第一个命中 */ }
```

匹配条件（关键）：

```cpp
namesp->_static == isStatic
  && AllowNative == (bool)namesp->native
  && namesp->name == sname
  && namesp->signature == ssig
```

模板默认值（`internal/method.h`）：`GetMethodID<bool isStatic, bool ReturnNull = false, bool AllowNative = false, bool trace = true>`。

### 3.2 注册侧（`FakeJni::Constructor` → `Class::Hook` → `FunctionBase::install`）

`FakeJni::Constructor<C, T...>` 经 `Descriptor(U&&, int flags = 0)` 走 `cl->Hook(env, "<init>", U::ctr)`，
`HookManager<FunctionType::None>` 以 **isStatic = true** 注册，签名由 `Wrapper::GetJNIStaticInvokeSignature` 生成：
`"(args)" + JNITypes<Return>::GetJNISignature(env)`，其中 `Return = std::shared_ptr<C>` → `L<nativeprefix>;`。

所以一个构造器在表里长这样（实测 dump）：

```
name='<init>' sig='(Lcom/unity3d/player/UnityPlayerActivity;Ljava/lang/String;)Lcom/mobge/assetlocator/AssetLocator;'
  static=1 native=(nil) handle=<非空>
```

**要点**：`static=1`、`native=nullptr`、`nativehandle!=nullptr`。`native != nullptr` 的项只由 **`RegisterNatives`** 产生（`vm.cpp`），二者的语义不同。

### 3.3 签名点号 vs 斜杠

Unity 的 `ReflectionHelper.getConstructorID` 把 `java.lang.Class.getName()` 的结果拼进签名，于是传进来的是**点号**形式：

```
(Lcom.unity3d.player.UnityPlayerActivity;Ljava/lang/String;)V
```

真实设备上没问题；但 jnivm 把字符串**原样**当查找键，而本仓库所有类都以斜杠形式注册 → 永不匹配。
→ 已在两处做归一化：`method.cpp` 的 `normalize_jni_sig()`（`L...;` 内 `.` → `/`）与 `javastubs/javac.cpp` 的 `bd_normalize_jni_sig()`（`Constructor` 构造时）。

---

## 4. 改动清单

### 4.1 建议保留（有实测收益）

| 文件 | 改动 | 依据 / 收益 |
|------|------|-------------|
| `libjnivm/src/jnivm/vm.cpp` | `RegisterNatives` 增进程级 keepalive 表（`bd_registered_method_keepalive()`），pin 住注册的 `Method` | 修"悬垂 jmethodID"：`UnregisterNatives` 释放后，同尺寸 `Method` 复用同一块内存，缓存的 id 会安静地变成一个**合法但错误**的函数。见 §11 |
| `libjnivm/src/jnivm/internal/method.cpp` | ① `<init>` 改写**去掉 `!isStatic` 守卫**；② 签名点号→斜杠归一化；③ **修 else 绑定 bug**（见 4.3） | ①Unity 的 `AndroidJNIHelper.GetConstructorID` 会走 static 重载，旧守卫使改写被跳过；③ 修复后 `DisplayMetrics`/`Handler` 等构造器首次能真正命中 |
| `javastubs/javac.cpp` | `Constructor` 构造时归一化签名 | 堵住 Unity 传来的点号签名 |
| `javastubs/bd_assetlocator.cpp`（新增） | `com.mobge.assetlocator.AssetLocator` 的 C++ 桩：注册 **4 种首参**的构造器（`JObject` / `Context` / `Activity` / `UnityPlayerActivity` + `String`）+ `ListAssets` | 游戏按 `UnityPlayerActivity` 首参查；`ListAssets` 按 `AssetManager.list` 语义返回**子项名** |
| `javastubs/android.h` / `android_content.cpp` / `android_descriptors.cpp` | 新增 `AssetLocator` 声明与注册；`Context::getApplicationContext()`；`AssetManager::list` 的 catch 分支补 `return` | 后者原本控制流落空（UB） |
| `projects/unityloader/javastubs/unity.cpp` | `ReflectionHelper::getMethodID` 的 **Method 1b（继承链 BFS 查找）** | 修 `getIntent`/`getAssets`/`getPackageManager` 等在**基类**上、却用泛型签名查导致返回 `(nil)` 的问题。⚠️ **该改动已在 HEAD 中，不属于 09-21 未提交批次** |
| `projects/unityloader/javastubs/unity.cpp` | **`ReflectionHelper::getConstructorID` 给返回的 `Constructor` 绑定执行体** | **P0 修法**。见 §4.4 / §6。**不涉 `libjnivm`，无全局回归** |
| `javastubs/android_view.cpp` | `Display::getRealMetrics` 对 `nullptr` 参数做保护 | 修 `SIGSEGV at fault addr 0x58`，该崩溃曾让进程在 `nativeRender` 后 2.6s 死掉 |
| `projects/unityloader/main.cpp` | **`bypass_video_translate` 按定稿契约实现**：回填 a1 = 本地路径、a2 = NULL、a3 = 文件长度、`return 1` | **修黑屏**（`BD_BYPASS_VIDEO_TRANSLATE` 打开时才装，默认路径不受影响）。契约推导与四次迭代见 §0.6-C/D/E，实测结果见 §0.6-H |

### 4.2 应清理（纯诊断，验证完就删）

| 位置 | 内容 |
|------|------|
| `libjnivm/src/jnivm/internal/method.cpp` | `[BD-CTOR-ENTER]`（`GetMethodID` 入口，只对 `<init>`）、`[BD-ANY-MISS]` + 注册表 dump（`[BD-MISS]`，在 miss 分支，会遍历该类全部 entries） |
| `libjnivm/src/jnivm/vm.cpp` | `[BD-REG]` 两条。**注意**：找到问题后建议把这行降级或删除，它在每次 `RegisterNatives` 都打印 |
| `javastubs/javac.cpp` | `Constructor::newInstance` 里的 `lookup ->` / `hit:` / entries dump（**`newInstance` 实际从未被调用**，见 4.4） |
| `projects/unityloader/javastubs/unity.cpp` | `getConstructorID` 里的注册表 dump（现多了一行 `bound ...`，一并清） |
| `javastubs/android_view.cpp` | `getRealMetrics` 里的 DisplayMetrics dump（**null 保护本身要留**，dump 可删） |
| `libjnivm/include/jnivm/method.h` | `BD_J2_TRACE` 开关（`j2invoke` 里打印 native 指针与签名）。诊断 fmod 悬垂 id 时加的，可留可删 |

### 4.3 ⚠️ 自己引入并已修复的 bug（教训）

把 `if(!isStatic && sname == "<init>")` 改成 `if(sname == "<init>")` 后，我又在**函数体内部**加了一层 `if (isVoidCtor)`：

```cpp
// ❌ 错误版本
if(sname == "<init>") {
    const bool isVoidCtor = ...;
    if (isVoidCtor) { ...改写...; return ...; }
}                       // ← 落到这里时既不改写也不 return → else 的查找被整个跳过
else { /* 查找 */ }
```

当签名**已经是改写形态**（即 static 重入那次）时就会落进 miss 分支。
**症状极具迷惑性**：`[BD-MISS]` 打印的查找键与注册项**逐字节相同**，却判定 miss。

**修法**：两个条件一起决定 `if` / `else`：

```cpp
const auto close = ssig.find(')');
const bool bdVoidCtor = close != std::string::npos &&
                        close + 1 < ssig.size() && ssig[close + 1] == 'V';
if(sname == "<init>" && bdVoidCtor) { ...改写...; return ...; }
else { /* 查找 */ }
```

**教训**：往一个"条件 + else"结构里塞内层判断时，务必确认内层判断为假时控制流去哪。

### 4.4 定论：`Constructor::newInstance` 从未被调用 —— 返回对象本身就是 jmethodID

`ReflectionHelper.getConstructorID(clazz, sig)` 返回的 `jnivm::java::lang::reflect::Constructor` 对象，
Unity **不会**调它的 `newInstance`（加了入口日志后确认 0 次调用），而是**把这个对象直接当 jmethodID 用**
（走 `method.cpp` 的 **static** `CallMethod` 分支，`[STUB-MISS] Unknown Static` 即出自这里，不是查找阶段）。

> 用探针把 `mid` 指针与返回值对齐后确认两者完全一致（`[BD-MID] mid=0x40030d3425f0`）：
> `javac.cpp` 的 `Constructor` 构造时**只填 `name`/`signature`**，`nativehandle`/`native`/`dynamic` 恒为空；
> 而 static `CallMethod` 只有 `mid->dynamic` 或 `mid->nativehandle` 两条成功路径 → 两空 → 落进 `Unknown Static` → 返回 null。
> **所以签名归一化（§3.3）是对的、也在生效，但不是根因**；表面症状（点号 + `V`、`handle=(nil)`）
> 全部由"包装对象没绑执行体"解释，不需要额外假设。

**实际修法**（`projects/unityloader/javastubs/unity.cpp` 的 `getConstructorID`）：按**参数列表**
（忽略返回类型——请求是 `(...)V`，注册项是 `(...)L<class>;`）在 `clazz->methods` 里找到真正带 body 的
`<init>` 条目，把执行体拷进这个包装对象：

```cpp
ctor->nativehandle = body->nativehandle;
ctor->native  = body->native;
ctor->_static = true;
```

不改 `libjnivm`，因此**没有全局回归风险**。

**教训**：判断"某个改动有没有用"时，要盯住**数据流的终点**，不能只看"哪条代码路径没被执行"。
`newInstance` 没被调用 ≠ 这个对象没用；它是"jmethodID 本体"而不是"查找工具"。
另外，`Unknown Static` 那行 `Sig=` 打印的是**被调用对象自己的 signature**，不要误当作"查找键"来推理。

---

## 5. 已验证的确定性结论

1. **`RegisterNatives` 确实被大量调用**：`org/fmod/FMODAudioDevice`、`com/unity3d/player/UnityPlayer`（20 个）、`ReflectionHelper`、`GoogleVrProxy`、`GoogleARCoreAPI`、`Camera2Wrapper`、`NativeLoader`、`bitter/jnibridge/JNIBridge`。
   **`android/util/DisplayMetrics` 上没有任何 `RegisterNatives`。**
2. **`DisplayMetrics` 的 `handle=0` 空桩**来源是 `GetMethodID` miss 分支的 `cur->methods.push_back()`，每条 miss 都会插一条（`find_if` 取第一个命中，所以空桩可能永久遮蔽后续注册项——这是真实隐患，见 §7.1-4）。
3. `<init>` 查找与注册的签名在修复后**能正确匹配**，`DisplayMetrics`/`Handler` 等的 miss 归零。
4. 帧仍全黑**不是**因为 `getRealMetrics` 崩（已修）**也不是**因为 present 没发生（14 次 swap），而是**场景里没有内容可画**；当前已定位到视频链（§0.6）。
5. **`ReflectionHelper.getConstructorID` 返回的对象就是"jmethodID 本体"，不是查找工具**（已修，见 §4.4）。
6. `FakeJni` 注册的构造器一律是 **static** 条目，签名为 `"(args)L<class>;"`（返回类型是类自己，不是 `V`）。
   因此按**完整签名**比对构造函数永远匹配不上；要比就比**参数列表**（截到 `)`）。写任何"手工解析 `<init>`"的代码时都要记住。

---

## 6. 阻塞点（按优先级）

### ✅ 已关闭（保留结论与待办）

| 曾经的 P0 | 结论 |
|---|---|
| `AssetLocator.<init>` 解析失败（2026-09-21） | 根因**不是**签名、**不是** `GetMethodID` 改写，而是 `getConstructorID` 交出去的包装对象**没有执行体**。修法见 §4.4，实测通过：`AssetLocator` 构造成功、`ListAssets('SoundBanks') -> 153 entries`。修复前后对照：`[STUB-MISS]` 56 → 55（**只少了这一条，零新增**），无异常/SEGV |
| `AssetLocator.GetReaderWrapper(String)` 没有桩（2026-09-21 晚） | 已用"惰性活对象"探针解掉阻塞：**NRE 归零**，8 个 AssetBundle **全部真实打开并打出大小**（`assets/Bundles/bundle1` 13 498 260 B … `bundle8` 4 256 915 B），`Unable to read header from archive file:` 从 **8 条降到 0 条**。⚠️ **仍只是探针**：内容还是默认值，且只有 `Seek(JI)J` / `Read(I)I` 签名是实测逼出来的（其余四个只有字面量池证据）。<br>待实现 API（`[BD-ANY-MISS] cls=java/lang/Object` 实测 + 字面量池交叉验证）：`Seek(JI)J`✅ / `Read(I)I`✅ / `GetBytes` / `GetLength` / `GetPosition` / `Close`（后四个未见调用，字面量 idx 5280/5283/5284/5278） |
| `[JNIVM] Invalid Reference, Unexpected Type`（2026-09-21 晚） | **不是** `JNIVM_ENABLE_RETURN_NON_ZERO=ON`（`CMakeCache.txt` 明确 OFF）；由**哨兵 `unity.toml`** 触发（§0.6-F 第 2 条） |

### 🔴 P0-b（当前主阻塞 —— 见 §0.6）

**症状**（补齐 NDK 符号**之前**的历史日志，留作对照）：

```
Unity: AndroidVideoMedia::OpenExtractor could not translate
       jar:file://!/assets/Videos/mobge_and_senri_splash_video.mp4 to local file.
Unity: AndroidVideoMedia: Error opening extractor: -10004
```

**事实核对**：
- 文件确实在 `gamedata/assets/Videos/mobge_and_senri_splash_video.mp4`（**979 746 B**）；
- NDK 符号链已补齐，`could not load symbol` 已消失 → 不再是"退回 JNI 路径"的问题；
- 翻译由 **Unity 原生代码**做，发生在它调 `setDataSource` **之前**（`libunity.so` 内 `0x4c124c`，错误串在偏移 `0xbde848`）；
- **URL host 为空的来源未找到**（`obb` 一支已排除，见 §0.6-F 第 1 条）。

**待办方向**已全部迁到 **§7.0**（0-a / 0-b / 1 / 2 四条），不再在本节重复。
兜底那一项（`thunks/ndk/media.cpp` 的 `bd_local_media_path()`，在 `AMediaExtractor_setDataSource` 入口把
`jar:file://<host>!/<asset>` 解成本地文件，候选顺序 `<host>/<asset>` → `<cwd>/<asset>` → `<cwd>/gamedata/<asset>`）
**至今没有机会生效**：它只在 host 非空时才有意义，而 Unity 在调 `setDataSource` 之前就放弃了。

### P0 —— 回归风险：`libjnivm` 是全局改动

`GetMethodID` 的核心行为（`<init>` 改写不再区分 `isStatic`、签名归一化）会影响**所有**其他移植项目。
早一批已跑过 4 组对照（§7.1-1），**本批新增改动尚未回归** —— 清单与做法见 §7.0-7。

### P1 —— 残留 `STUB-MISS`（去重后各 1 条）

> **按目标约束，本节大部分不需要修。** 只处理「会升级成致命异常」的；
> billing / 网络 / 云服务类的返回安全默认值即可，别实现功能语义。

构造器类：
- `com/mobge/oddmarbilling/OddmarPurchaseHandler.<init>(UnityPlayerActivity, UnityPlayerActivity)`
- `android/media/AudioFocusRequest$Builder.<init>(I)`
- `android/media/AudioAttributes$Builder.<init>()`
- `java/lang/Object.<init>()Ljava/lang/Object;`（形态奇怪，尾部是 `Ljava/lang/Object;` 而非 `V`，值得单独看）

实例方法类：
- `com/unity3d/player/UnityPlayerActivity.getPackageManager()`
- `android/view/Window.getAttributes()`
- `android/view/SurfaceView.{setOnApplyWindowInsetsListener, onApplyWindowInsets, getRootWindowInsets, addOnLayoutChangeListener}`
- `android/os/Bundle.getBoolean(String)`
- `android/media/AudioManager.{requestAudioFocus, isBluetoothA2dpOn}`
- `android/hardware/display/DisplayManager.registerDisplayListener`
- `android/content/Intent.getExtras()`
- `android/app/Activity.{getPackageManager, getObbDir, getObbDirs, getAssets}`
  （⚠️ `getObbDir` / `getObbDirs` **已在 `Context` 与 `Activity` 两侧实现并注册**；若日志里还见它们的 miss，
  说明是**旧的调用点缓存了空桩**，见 §5 第 2 条）
- `java/lang/StackTraceElement.<init>(...)`、`java/lang/Error.<init>(String)`

> 注：`getAssets`/`getPackageManager`/`getIntent` 这类"在基类上"的，理论上已被 Method 1b 覆盖，仍 miss 说明 **1b 未覆盖到该调用点**，值得复查。

#### ⚠️ 已核实：`getApplicationContext` 的修复**没有生效**，别照着它抄

`javastubs/android.h` + `android_content.cpp` 新增了 `Context::getApplicationContext()` 并注册，
但修法解决不了问题：

```
查找键（Unity） : str0='getApplicationContext' str1='()Ljava/lang/Object;'   static=0 与 static=1 都试过
注册项（我们） : name='getApplicationContext' sig='()Landroid/content/Context;' static=0 handle=1
```

**两侧签名不匹配 → 照旧 miss。** 实测 miss 次数 **before = 72 / after = 72，一条没少**。
→ 真要修，得处理 **「Unity 拿泛型签名 `()Ljava/lang/Object;` 查具体返回类型」** 这个模式
（同类还有 `findClass`/`getMethodID` 的泛型回退），而不是再注册一个具体签名的重载。
**当前判断：与黑屏无关，先不动。**

#### 同样没被触发的三处（想省时间就别再查它们）

| 改动 | 实测状态 |
|------|---------|
| `StackTraceElement` + `HookThrowableExtensions` | hook 挂上了 11 个类，Unity **0 次调用** |
| `getRealMetrics` 的 null 保护 | `"getRealMetrics: metrics is NULL"` **0 次** |
| `__android_log_*` 改走 `BD_LOG` | `[ANDROID]` **0 条**，游戏压根没走这些入口 |

它们是"代码正确但现象未验证"，**不是**已确认有效的修复。

### P2 —— 音频与其它

- `WwiseUnity: Failed to initialize the sound engine. Reason: AK_Fail`（不影响画面，先放着）
- `Firebase SWIGRegisterExceptionCallbacks_AppUtil EntryPointNotFoundException`
- `fb0 unavailable fd=-1`（无 framebuffer 设备，不影响 llvmpipe GL present）

---

## 7. 后续任务（按优先级）

### 7.0 ⭐ 从这里开始

> 前置：**先做 §⛔ 接手第 0 步**（核对/还原 `unity.toml`、确认 `BOOT_LOADER`、确认二进制里真有你要的字符串）。
> **动手前先读 §0.6**：那里已经把那个 Unity hook 的调用契约钉死了，别再重复踩。
> 判读任何一轮，同时看 **`ZZZSENTINEL`（必须 0）** 与 **`Unable to read header`（必须 0）**；
> ⚠️ **`init time` 不能用来判死活**（理由见 §9.2 的更正）。

**剩余待办（速览）** —— 与 §0.2 总表的 L# 一一对应

| # | 任务 | 优先级 | 前置 / 判据 | 详细步骤 |
|---|------|--------|------|------|
| **L5** | 本批改动的回归：`libjnivm`（`method.cpp` / `vm.cpp` / `findclass.cpp`）+ `thunks/ndk/*` + `getPackageCodePath` | **最高** —— `libjnivm` 是**全局改动**，影响所有端口 | 用 Samurai2 / Maximus2 各跑一对（包内自带 loader = 改动前基线） | §7.0-7 |
| **L3** | 摸清 `0x4c124c` 背后的实体类型（`[a0+0x410]` → `vtbl[0x138]`），以及另 3 个调用点是否同链 | 中 —— 只有**长期保留**该 hook 时才必要 | 先决定要不要把 hook 固化 | §7.0-2 |
| **L6** | `AssetLocator.GetReaderWrapper` 从探针改为真实读出内容 | 中 | 当前不阻塞主线（8 个 bundle 已有真实大小） | §6 P0 |
| **L4** | 云服务回调"不阻塞化"（`SocialImpl.authenticate` / `SaveGames.isConnected`） | 低 —— 目标约束下只需不阻塞 | **不要**实现功能语义 | §0.5-E、§7.0-5 |
| **L7** | §4.2 诊断日志清理、`android_content.cpp:934` 过期注释、探针生命周期 | 低（收尾） | 建议在回归通过后再动 | §7.0-6/8、§7.1-9/10 |

> 容器侧的清理**已于 2026-09-22 收尾完成**（见 §9.3），不再占待办。

**第 0 组**

0-a. ✅ **已完成（2026-09-22 晚）—— `BD_BYPASS_VIDEO_TRANSLATE` 的 hook 已改成真实 ABI**
   （代码 `projects/unityloader/main.cpp` 的 `bypass_video_translate`；完整契约与四次迭代见 §0.6-C/D/E）。
   验收全部达成：`could not translate` 消失 → `open` 成功（`fd=30`）→ `offset=0` → `extractors opened tracks=2`
   → `decoded frame=0/1` → **截图 290–300 KB = Oddmar 标题界面**（§0.6-H）。
   - ⚠️ 提醒：`build-oddmar-verbose` 才含这个 hook；`build-regress` 里相关字符串是 0（`grep -c` 自查）。
   - 若要"根治"（让 Unity 自己算出路径、去掉这个 hook），转 0-b 与第 1 组。

0-b. ❌ **已废弃 —— "只改 `android_package_code`"的 A/B 是 no-op**（2026-09-22 深夜结论，见 §0.7-B）。
   `bd_compute_source_dir()` 在 staged APK 缺失时走 `android_source_dirs` 分支，最终落到
   `logical = <cwd>/UnityDataAssetPack.apk`，**与 `android_package_code` 取值无关**；
   只有把新值写成**以 `.apk` 结尾**的字符串才会改变返回值 —— 那样也测不出"host 来源"，因为
   §0.7 已经把来源查清了（`Application.dataPath`）。
   **别再按原方案跑这一轮。** 想验证 host 机制请直接读 §0.7。

**第 1 组（0-a 通过后再做）**

1. ✅ **已接线并已给出结论（2026-09-22 深夜）**。探针 `BD_PROBE_APPPATHS=1` 打印：
   `Application.dataPath = ''`、`streamingAssetsPath = 'jar:file://!/assets'`（**Unity 自己就报 hostless**）。
   `libunity` 侧的链也走完了：`0x39f800 → 0x4bbe14 → 0x4bfbe8 → vtbl[0x170] → libunity+0x30c704`
   = `return std::string(g_appPathBuf)`（`g_appPathBuf` @ `libunity+0xF0B740`）。
   上游唯一写入者 `SetDataPathChecked()` 被 **`stat()` + `S_IFREG`** 把关，而候选来自
   `getPackageCodePath()` = `<cwd>/UnityDataAssetPack.apk`（不存在）⇒ `dataPath` 留空。
   **完整链条、实验与"根治"可行性评估见 §0.7。**
2. **顺着 4 个调用点摸清 `0x4c124c` 背后的实体类型**（`[a0+0x410]` → `vtbl[0x138]`）。
   若要长期保留这个 hook，需要确认它是否只服务于视频链（另三个调用点 `0x5452dc` / `0x78b81c` / `0x78d534`）。

**第 2 组（第 1 组跑通后再动）**

4. 画面出内容后，再回来看 `frame.*.gl.ppm` 是否出现非零像素；若仍黑，转查相机/场景/AssetBundle 挂载。
5. 云服务回调（§0.5-E）：确认 `SocialImpl.authenticate` 是否会卡住 `MGLOProgressData.construct` 链。
   断网约束下只需"不阻塞"，**不要**实现功能语义。
6. `[BD-ANY-MISS]` 噪声降级、§4.2 诊断日志清理、`GetMethodID` miss 时 `push_back` 空桩遮蔽后续注册
   的结构性隐患（这条是 `libjnivm` 全局改动，做之前先补回归）。

**第 3 组（本批改动收尾）**

7. **`libjnivm` / 全局改动的回归**：本批累计改了 `method.cpp`、`vm.cpp`、`findclass.cpp`、
   `javastubs/android*.cpp`、`thunks/ndk/*`。§7.1-1 的 4 组对照是**更早一批**的结论，
   本批新增改动（尤其 `thunks/ndk/*` 的 `AMediaFormat` 结构体加了 `buffers`/`text` 字段、
   `getPackageCodePath` 改走 `bd_compute_source_dir()`）**还没回归过**，建议用 Samurai2/Maximus2 补一对。
8. ✅ **容器清理已完成（2026-09-22 收尾）**：`seq*` 由 69 个降到 3 个、`/tmp` 反汇编中间件已删、
   12 个过程日志已删（合计回收约 330 MB）；`log*.bak` 5 个作对照基线**有意保留**。清单与理由见 §9.3。
   - 宿主机那份"半成品探针"**已接线**（`BD_PROBE_APPPATHS=1`），见 §0.3 / §0.7。

### 7.1 此前列出的（压缩：✅ 已完成 / ⬜ 未做）

1. ✅ **回归已完成（2026-09-21）**，用两个**已发布端口**做真对照（`Samurai2` / `Maximus2`，
   包内自带 loader = 改动前基线；另编 `build-regress` = 改动后；同容器、同 Xvfb、`SECS=60`）：
   **4 组全部跑满 60 s、零 `[BD-SEGV]` / `terminate`**，两个游戏都到 GL 初始化，
   `nativeRender ×22`、`eglSwapBuffers ×24–29`；Samurai2 的插件 `samurai2_offline.so` 正常 `init`
   （**plugin ABI 未变**），Maximus2 `PlayerPrefs` 存下 127 条。→ **这批改动没有破坏既有端口**。
   复现：容器内 `/regress-run.sh`、`/regress-all.sh`（用法见脚本头注释）。
2. ✅ **定位 `AssetLocator` 那个 mid 的来源** → `mid == getConstructorID` 返回值、`handle=(nil)`；根因见 §4.4。
3. ✅ **加长运行时间，把"黑屏"和"没跑够"分开** → `SECS=90` / `SECS=120` 都跑满、210–227 帧，
   画面依然 192 B 纯色 ⇒ 不是没跑够，是真的没有内容可画。摘掉 Wwise 复跑的对照**仍未做**，但优先级已降。
4. ⬜ **修结构性隐患**：`GetMethodID` miss 时无条件 `push_back` 空桩，会让第一个 miss 永久遮蔽同名同签名的**后续**注册（`find_if` 只取第一个）→ 改成"复用同名同签名条目并填充"。**`libjnivm` 全局改动，做之前先补回归。**
5. ⬜ **`[BD-ANY-MISS]` 噪声降级**：它在 `GetMethodID` miss 分支**无条件**打印，而 `Class::getMethod` 会用
   "member + static"双向探测，所以**一次成功的** `getMethod` 也会吐 1~2 行 miss
   （`Choreographer$FrameCallback.doFrame`、`JNIBridge.invoke` 每帧都刷）。日志从 26528 涨到 29536 行，
   增量几乎全是它。建议加 `if(trace)` 或改成计数摘要。
6. ✅ **处理 P1 里与主循环相关的 `Activity.getObbDir(s)`** → 已实现（`android_misc.cpp:175-176`、`android_content.cpp:797-826`），
   但**没解决 host 为空**（§0.6-F 第 1 条）。其余 `Intent.getExtras`、`Bundle.getBoolean`、`Window.getAttributes`、`SurfaceView.*` 仍留在 P1。
7. ⬜ 画面真正出内容后，再看 `frame.*.gl.ppm` 是否出现非零像素；若仍黑，再查相机/场景/AssetBundle 挂载。
8. ⬜ 清理 §4.2 的诊断日志（`getConstructorID` 的注册表 dump 含新增的 `bound ...` 行，一并清）。
9. ⬜ **`javastubs/android_content.cpp:934` 的注释**（"getObbDir / getObbDirs -> STUB-MISS path returns null"）
   已与上面 100 行的实现矛盾，顺手改掉——全仓现在只剩这一处过期注释。
10. ⬜ **探针生命周期管理**：`video translate args` 这类一次性诊断在拿到定论后应降级或门控。
    当前它只在 `BD_BYPASS_VIDEO_TRANSLATE` 打开时才走，**符合"默认路径不受影响"的要求，保持这个形态**。

---

## 8. 诊断开关速查

| 开关 | 作用 |
|------|------|
| `BD_ENABLE_LOG`（CMake，默认 OFF，上机 ON） | 主日志总开关 |
| `BD_ENABLE_TRACE`（CMake） | `BD_DEBUG` / `BOOT_LOG` 等 |
| `BD_ENABLE_VERBOSE`（CMake） | 大量 `verbose()`（`build-oddmar-verbose` 已开） |
| `BD_JNI_TRACE=1`（env） | **放开 libjnivm 全部日志**（含被过滤器挡住的），量极大 |
| `BD_J2_TRACE=1`（env） | `j2invoke` 打印 native 指针 + 签名（查悬垂 id 用） |
| `BD_DUMP_FRAME` / `BD_DUMP_FRAME_AT`（env） | 让 loader 用 `glReadPixels` dump 自己的 drawable 到 `.gl.ppm` |
| `BD_MEM_LOG_MS`（env）/ `[debug] mem_log_interval_ms`（toml） | `[BD-MEM]` 间隔，默认 2000ms，`0` 关闭 |
| `BD_CTOR_FALLBACK_NULL=1`（env） | 让 `getConstructorID` 恢复"返回 null"的旧行为（逃生开关） |
| `BD_PROBE_APPPATHS=1`（env） | ✅ **已接线**（2026-09-22 深夜）。打印 `[BD-PATH-PROBE] Application.dataPath/streamingAssetsPath/...` 真值，外加 `[BD-L2]` 的 `vtbl[0x170]` 解析。`post-init` 点偏早，**`video` 点（挂在 `BD_BYPASS_VIDEO_TRANSLATE` 的 hook 里）更可靠** |
| `BD_BYPASS_VIDEO_TRANSLATE=1`（env） | 在 `libunity.so + 0x4c124c` 装 detour，**代替 Unity 完成 asset 路径翻译**（回填本地路径 + 给出数据长度）。**默认不装**。✅ 2026-09-22 晚起契约正确、实测画面可出，见 §0.6-E/H |

**自检命令**（确认二进制里真的有你以为的代码 —— 这一条已救过两次）：

```bash
U=/workspace/Bogodroid/build-oddmar-verbose/unityloader
grep -c "bypass: out path" $U   # 2026-09-22 晚实测：1（当前实现；build-regress = 0）
grep -c "bypass: a1 obj"   $U   # 同上：1
grep -c "PATH-PROBE"       $U   # 实测：4
grep -c "singleton@"       $U   # 2026-09-22 深夜实测：1（L2 探针，见 §0.7）
# 二进制 mtime 2026-09-22 10:15、153 402 768 B = 含 L2 探针的那版
```

> ⚠️ **`grep` 二进制字符串只证明"代码在里面"，不证明"它打过日志"**。
> 本轮就踩过：源码里有 `video translate args`，但最后一次运行用的二进制**还没有**这两行
> → 全仓日志 `grep -c` = 0，白等一轮。**跑完先 `grep -c` 日志，别只 grep 二进制。**
> （`video translate args` 那个字符串在 v4 重写时已移除。）

**构建目录速查**：

| 目录 | 类型 | 关键开关 | `unityloader` 时间/体积 | 含视频 bypass？ |
|------|------|---------|----------------------|---------------|
| `build-oddmar-verbose` | **在用** | RelWithDebInfo / LOG=ON / VERBOSE=ON / NON_ZERO=OFF | 09-22 05:12，153 MB | ✅ hook + args 日志 |
| `build-regress` | 回归对照 | RelWithDebInfo / LOG=ON / VERBOSE=OFF / NON_ZERO=OFF | 09-21 14:31，152 MB | ❌ 两个字符串都是 0 |
| `build-oddmar` | **旧产物，勿用** | Release / LOG=ON | 09-21 07:55，6.3 MB | ❌ |
| `build-oddmar-sym` | 备用 | RelWithDebInfo | 09-21 08:04，144 MB | ❌ |
| `build-anbernic-debug` | 上机调试 | Debug / LOG+TRACE=ON | 09-16 08:33，102 MB | ❌ |
| `build-anbernic-rel` | **发版用** | Release / LOG=OFF | 09-18 07:22，6.2 MB | ❌ |

> 七个目录的 `JNIVM_ENABLE_RETURN_NON_ZERO` 实测**全部为 `OFF`**（已逐个 grep 过 `CMakeCache.txt`）。
> 即便如此，**每次构建仍要显式带 `-DJNIVM_ENABLE_RETURN_NON_ZERO=OFF`**（AGENTS.md 的 CMake 缓存陷阱）。

---

## 9. 容器内可复用资产与待清理项

### 9.1 脚本（都在容器 `/` 下，直接 `bash /xxx.sh` 调用）

| 脚本 | 用途 |
|------|------|
| `/run-oddmar-seq.sh` | **主用**。逐帧截 X 根窗口 + loader `glReadPixels` dump。可用 env 覆盖：`SECS`（默认 32）、`SEQ_DIR`、`BOOT_LOADER`、`BD_DUMP_FRAME_AT` |
| `/run-any-seq.sh` | 通用版（换游戏用），同上参数 |
| `/run-oddmar.sh` | 精简版（无逐帧截图） |
| `/regress-run.sh` / `/regress-all.sh` | 回归对照（Samurai2 / Maximus2），用法见脚本头注释 |
| `/probe-oddmar-window.sh` | X 窗口探测 |

### 9.2 标准运行命令（照抄即可）

```bash
docker exec GlES_Dev bash -lc '
  cp /game/Oddmar/unity.toml.bak-rd9 /game/Oddmar/unity.toml     # 先还原（§⛔ 0-1）
  cd /game/Oddmar &&
  GAME_ROOT=/game/Oddmar SEQ_DIR=/game/Oddmar/seq-rd10 SECS=120 \
  BOOT_LOADER=/workspace/Bogodroid/build-oddmar-verbose/unityloader \
  BD_BYPASS_VIDEO_TRANSLATE=1 \
  bash /run-oddmar-seq.sh > /game/Oddmar/rd10.out 2>&1; echo "exit=$?"'
```

判读四件套（**必须同时看**，否则会误判）：

```bash
L=/game/Oddmar/log-seq.txt
grep -c ZZZSENTINEL $L                    # 必须 =0；非 0 ⇒ toml 是哨兵态，本轮作废（§⛔ 0-1）
grep -c "Unable to read header" $L        # 必须 =0；且 grep -c "BD-ASSETLOC" 应 >0
awk '{print $4}' $SEQ_DIR/timeline.txt | sort -u | head   # 全是 192 就是没画面
grep -a -n "bypass: out path\|video translate in-URL\|-10004" $L | head
```

> ⚠️ **`init time` 不能用来判死活**（2026-09-22 深夜更正，别再照抄旧版）：
> 成功跑通的一轮（`./` 本地目录模式）`init time` 读数同样是 **0**，与哨兵态的死法数值重合。
> 判"活着"只看两条 **`ZZZSENTINEL` = 0** 且 **`seq*/timeline.txt` 不止 192 B**（192 B = 纯色 = 没画面）。
> 若非要看 `init time`，它只在"是否走到 il2cpp 初始化"这一个维度上有意义，不能反推成败。

### 9.3 待清理（**✅ 2026-09-22 收尾已执行** —— 执行记录见本节末）

- **（收尾前）`/game/Oddmar/seq*` 共 69 个目录**（`seq`、`seq2`…`seq90`、`seqBASE/RD2/REV/SPLASH`…、
  `seq-rd5`…`seq-rd9b`、`seq-obb-test*`、`seq-jar-bypass*`、`seq-hook-test*`、`seq-args1/2/3`）：
  全部是历次运行的截图与 timeline，**没有源头价值**（`seq-args3` 想留就留，它是 hook 入参那轮的产物）。
- **`/game/Oddmar/log*.bak` 7 个**（含 11.9 MB 的 `log-seq.seq90.bak`）。
  其中 **`seqREV.bak` / `seq28.bak` 是 `getConstructorID` 修复的前后对照，`before90.bak` 是回归基线** ——
  确认不再需要再删。
- **2026-09-22 新增日志**（可清理，但 `log-args3.txt` 建议留到 hook 修好为止）：
  `log-obb-test*.txt`、`log-jar-bypass*.txt`、`log-hook-test*.txt`（4 个，每个 4.7–6.3 MB）、
  `log-args1/2/3.txt`、`args1/2/3.out`。
  其中 **`log-args1.txt` / `log-args2.txt` 是"哨兵态必死"的对照证据**，`log-args3.txt` 是 hook 入参证据。
- ⚠️ **`/game/Oddmar/unity.toml.bak-rd9` 不要删**：它是唯一一份正常配置备份。
- ⚠️ **`/game/Oddmar/unity.toml.sentinel-0922` 不要删**：哨兵版留档，做 A/B 对照用。
- 容器里那个 0 字节的 `/tmp/ZZZSENTINELPACK.apk` 留着无所谓，但**别把它当真实 APK 推理**。
- **2026-09-22 深夜新增（L2 场，建议留着当证据）**：`log-l2probe.txt`（健康轮 + 探针输出，25 MB）、
  `log-l2apk.txt`（占位 APK 轮，启动死的对照，89 KB）、`l2.out` / `l2apk.out`、
  `seq-l2/`（51 帧，终态 ~298–301 KB）、`seq-l2apk/`（51 帧，恒 192 B）。
  反汇编临时件在 `/tmp/`：`uni.asm`（96 MB）、`relocs.txt`、`xref.py` / `grab.py` / `findg.py`。

清理建议（**先只列、确认后再删**）：

```bash
# 1) 先看要删什么（这一步不能省：下面 `seq-*` 会连 seq-args3 一起匹配）
docker exec GlES_Dev bash -lc 'ls -d /game/Oddmar/seq* | wc -l; ls -d /game/Oddmar/seq-*; ls -la /game/Oddmar/log*.bak'

# 2) 确认无用了再删。注意两点：
#    - `seq-*` 会命中 seq-args1/2/3（其中 seq-args3 是 hook 入参证据，想留就先 mv 出去）
#    - 早期那批没有统一前缀，逐个列名删更稳
docker exec GlES_Dev bash -lc 'mv /game/Oddmar/seq-args3 /tmp/keep-seq-args3 2>/dev/null; \
  rm -rf /game/Oddmar/seq /game/Oddmar/seq[0-9]* /game/Oddmar/seq-*'

# 3) 日志备份：只删确定不再需要的，保留 unity.toml.bak-rd9 / sentinel-0922
#    seqREV.bak / seq28.bak = getConstructorID 修复前后对照；before90.bak = 回归基线
```

#### ✅ 2026-09-22 收尾：清理已执行

本节上面的清单按下面的口径**已跑过一遍**（数字为实测）：

| 对象 | 处理 | 结果 |
|------|------|------|
| `/game/Oddmar/seq*` | 删 66 个，留 3 个 | **69 → 3**（`seq-hook4` / `seq-l2` / `seq-l2apk`），回收 124 MB |
| `/tmp/uni.asm`、`unity.text.asm`、`relocs.txt`、`xref.py`/`grab.py`/`findg.py` | 删 | 反汇编中间件，**两条命令即可重生**（见 §13） |
| `/tmp/skul_run2.log`、`skul_run3.log` | 删 | 已止损项目的历史日志，149 MB |
| `log-obb-test*`（6）、`log-hook-test*`（4）、`log-jar-bypass*`（2） | 删 | 约 58 MB，结论已固化进 §0.5-C / §0.6-D/E |
| **有意保留** | — | `unity.toml.bak-rd9`、`unity.toml.sentinel-0922`、`log*.bak`（实测 **5** 个，非 7 个）、`log-args1/2/3.txt`、`args1/2/3.out`、`log-hook4.txt`、`hook4.out`、`log-l2probe.txt`、`log-l2apk.txt`、`l2.out`、`l2apk.out`、`log-seq.txt` |
| `/tmp` 里的素材与二进制 | **未动** | `maximus2.zip`(135 MB)、`samurai2.zip`(49 MB)、`libunity.so`、`ul.so`、`bd-unityloader`、`regress/` —— 删了要重新提取/提取，留着更省事 |

> 保留项里 `log-args1.txt` / `log-args2.txt` 是「哨兵态必死」对照证据，`log-args3.txt` 是 hook 入参证据，
> `log-hook4.txt` + `seq-hook4/` 与 `log-l2probe.txt` + `seq-l2/` 分别是 L1 / L2 的原始验收材料。
> 都不再需要时再删。

---

> **案例存档说明**：原 `docs/CASE_STUDIES.md` 中与 Oddmar 有关的三个小节已并入下方 §10–§12
> （标题保留原文的"案例四 / 四·续 / 四·续三"编号，便于与 `.workbuddy/memory/` 的工作日志对上）。
> 同文件其余三个案例（**Skul / Maximus2 / FiveHearts**）随该文件一并删除，可复用结论保留在
> `docs/PORTING_PLAYBOOK.md` §0 / §1.2 与 `docs/FIVEHEARTS.md` §4.6 B。需要原文时用
> `git show eb68cf7:docs/CASE_STUDIES.md`，或找回删除前的回收站副本。

---

## 10. 案例 A（原案例四）：Oddmar 启动即退出 —— 已解决

> **结局**：真因是 pending Java 异常 + 空 `std::exception_ptr`；修完 SEGV 消失，进程从此跑满观测窗口。
> 保留下面的**判据**与**教训**，过程排查已删。

### 最大的一课：`[BD-SEGV]` 的 backtrace 会骗人

当时把崩因判成「`libunity+0x319984` 空指针」，**是错的**，原因有两层：

1. **Unity 自己注册 crash handler**：`libunity` 的 `JNI_OnLoad` 会对 SIGSEGV 等装自己的 handler（`libunity+0x3196bc`），
   **覆盖** `main()` 里早先装的 `segfault_handler()`；它随后**链式回调**旧 handler（`libunity+0x319b2c`），
   所以我们最终仍能拿到信号——但栈已经不属于我们了。
2. **那个 handler 把 TLS 基址放在 `x29`**（`mrs x29, tpidr_el0`），不是 frame pointer。
   glibc 的 `backtrace(3)` 走 frame pointer 链，于是把 TLS 内存当栈帧读，
   吐出一个**落在 Unity 自己 handler 里的假外层帧**。`0x319984` 只是 `0x319980: bl 0x319b2c` 的返回地址，不是故障点。

**判据（一眼可辨）**：假帧地址与日志里 `[SIGNALS] handler 0x...` 打印的 handler 地址只差几百字节
（本例 `0x319984` 距 `0x3196bc` 仅 0x2C8），且 `[SIGNALS]` 段能看到 Unity 对 signum 11 的 install。

**修法（已实现）**：`print_backtrace_on_segfault()` 改用 `sigaction` + **SA_SIGINFO**
（Unity 的 handler 会把 `siginfo`/`ucontext` 原样转发过来），在 handler 里直接读
`uc_mcontext.pc / regs[29] / regs[30] / sp` 与 `si_addr`，再用 `bd_describe_crash_address()` 解析成 `module+offset`；
栈回溯改用 `bd_dump_crash_backtrace()`（fp 链 + 保守栈扫描 + 只认 call site 前一条指令），不再依赖被污染的 frame pointer。

### 真因与修法

```
[BD-SEGV] signal 11 si_code=1 si_addr=0xffffffffffffff80
BD-SEGV:   pc /tmp/bd-unityloader+0x46fd7c     →  addr2line: std::rethrow_exception
```

反汇编该点：`sub x1, x20, #0x80` / `ldaxr w0, [x1]` —— x20（exception object）为 **0**。
libstdc++ 的 `rethrow_exception` 要读异常对象**下方 0x80 字节**的 `__cxa_exception` 头，
空 `exception_ptr` 就变成读 `0 - 0x80 = 0xffffffffffffff80`。

链路：Unity 的受管异常路径会 new 一个 `java/lang/Error` 并 `JNIEnv::Throw()` 交给 jnivm，
但 `Throw()` 只是把 throwable 存进 `env->current_exception`，**没人填它的 `except`**；
下一次 JNI 调用走到 `j2invoke` 尾部就 rethrow 空指针 → SEGV。
（语义依据：**Android 上 `Throw()` 只是置起 pending 异常，调用本身正常返回**；
把异常解栈进游戏的原生帧只会 `std::terminate()`。）

修法（已实现）：
- `libjnivm/include/jnivm/throwable.h`：新增 `jnivm::RethrowThrowable()`，空 `except` 时抛普通 `std::runtime_error`；
- `libjnivm/include/jnivm/method.h`：`j2invoke` 只在 `except` 非空时 rethrow；
- `libjnivm/src/jnivm/vm.cpp`：`Throw()` 保持 `except` 为空（即 "pending"），`ExceptionDescribe` 走 `RethrowThrowable()`。

### 黑屏截图 ≠ 渲染成功：先证明 `eglSwapBuffers` 被调过

`run-oddmar.sh` 的截图点在 `SECS-2`。SECS=30 时它等到 28 s 才截图，而 Unity 可能 3.9 s 就 `nativeRender -> false` 退出
—— **窗口早没了，抓到的是空的 root window**。这层解释只说明"这张图没意义"，不是"画面是黑的"。

要看真东西，用 `run-oddmar-seq.sh`：进程存活期间每 250 ms 抓一张，同时 `BD_DUMP_FRAME` 让 loader 自己
`glReadPixels` GL drawable（`BD_DUMP_FRAME_AT=1` → swap #1/#2/#3 各出一张 `.gl.ppm`）。判据：

| 观察 | 结论 |
|------|------|
| 有 `.gl.ppm`，且 root 截图黑 | 画了但没 present（present 通道问题） |
| 没有 `.gl.ppm` | `eglSwapBuffers` **一次都没被调用**，Unity 根本没画完一帧 |
| 两者都有内容 | 渲染+present 正常，问题在别处 |

**192 B 这个体积本身就是判据**：640×480 单色图压完就这么大，几十 kB 才说明有内容。

**两个容易踩的点**：
1. **`BOOT_LOADER` 有默认值**，指向 `build-oddmar/unityloader`。只重建了 verbose 而忘了传 `BOOT_LOADER` 时，
   跑的是老二进制，会看到"崩溃又回来了"的假象（`[BD-SEGV]` 还是旧格式 `backtrace_symbols_fd` 的裸地址）。
   判断方法：新 handler 打 `si_code`/`si_addr`/`pc`/`lr`，旧的只打一行 `[BD-SEGV] signal N`。
2. **`xwininfo -root -children | grep -c '^     0x'` 数的就是子窗口数**，0 → 还没有 X 窗口
   （SDL 窗口是在第一次 `nativeRender` 内部创建的），用来把"窗口没建"和"窗口建了但没内容"分开。

### 通用教训

1. **不要相信掌机日志里的裸 backtrace**。先拿 `pc`/`si_addr`，再反汇编，别从 `backtrace_symbols` 的帧序推因果。
2. **`rethrow_exception(空 exception_ptr)` 是致命而非"抛个空异常"**，故障现象是 `si_addr=0xffffffffffffff80`（= `-0x80`），与游戏代码无关。
3. **Unity 会抢信号处理器**。要保自己的 SEGV handler，得在 Unity `JNI_OnLoad` 之后再装一次，
   或让 `sigaction_impl` 拒收游戏对 SIGSEGV 的替换；现在靠 SA_SIGINFO 从链式调用里拿 ucontext 已够用。
4. `open("")` 有专门诊断（`bd_dump_crash_backtrace("open-empty")`）。Oddmar 有 14 次，全来自 `libunity+0x8a2b58`，
   是良性探测（拿 -1 继续走），别当成崩因。

---

## 11. 案例 B（原案例四·续）：悬垂的 `jmethodID` —— 已解决

> 症状：`[BD-DBUF] GetDirectBufferAddress handle=0x4 from 0x3800afb760` 处 SIGSEGV。
> `0x3800afb760` = `libunity + 0xafb760`，属于 `fmodProcessMicData`，而调用方是
> `FMODAudioDevice::local_fmodGetInfo(4)` —— 它取 `fmodGetInfo` 的地址去调，`4`（`infoId`）
> 被当成了 `ByteBuffer` 句柄。

**排除法结论**（过程省略，四步都已验证）：表本身没串号（按 24 字节步长扫 libunity 的 `JNINativeMethod`
三元组，`fmod*` 只有三条，无重复注册）：

| 表项偏移 | name | signature | fnPtr |
|---|---|---|---|
| `0xe9d008` | `fmodGetInfo` | `(I)I` | `0xafb5e4` |
| `0xe9d020` | `fmodProcess` | `(Ljava/nio/ByteBuffer;)I` | `0xafb6ac` |
| `0xe9d038` | `fmodProcessMicData` | `(Ljava/nio/ByteBuffer;I)I` | `0xafb738` |

另外三点：`fmodGetInfo` 反汇编后**一条 `blr` 都没有**（不可能调 JNI）；`0xafb760` 确实属于
`fmodProcessMicData`（`blr x8` 在 `0xafb75c`）；jnivm 侧注册与文件表完全一致。

### 真因：对象被释放后同尺寸分配复用了

在 `local_fmodGetInfo` 里每次调用都打印缓存的 `Method*` 和它的 `native`，两次 `runAudio` 之间出现：

```
1128: local_fmodGetInfo(id=0) mid=0x4001e15da420 native=0x3800afb5e4   ← 第一次正常
1317: [FMODAudioDevice] Exiting audio loop.
1320: local_fmodGetInfo(id=0) mid=0x4001e15da420 native=0x3800afb738   ← 同一个指针，值变了
1322: local_fmodGetInfo(id=4) mid=0x4001e15da420 native=0x3800afb738   ← 于是崩
```

**同一个 `Method` 对象的 `native` 字段换了值**，而且换成了一个合法的、就在表里紧挨着的函数地址 ——
不是内存被写花，是**对象被释放后同尺寸分配复用了**：

- jnivm 的 `jmethodID` 就是裸的 `Method*`；`RegisterNatives()` 把每个原生方法 `make_shared<Method>()` 后
  `push_back` 进 `Class::methods`（`vm.cpp`）；
- `UnregisterNatives()` 会把 `native != nullptr` 的方法从 `methods` 里 `erase` → 最后一个 `shared_ptr` 掉了
  → **方法对象被 free**；
- 下一次同尺寸的 `Method` 分配（重新注册、或任何 auto-stub 方法）**复用了同一块内存**，
  于是那个缓存的 id 还指向有效地址，却安静地变成了另一个 native 函数。

`FakeJni::MethodProxy` 正是那种"解析一次、缓存 `jmethodID`"的调用者，
`local_fmodGetInfo` / `local_fmodProcess` 里的 `static auto` 也是。

### 修复

1. **`libjnivm/src/jnivm/vm.cpp`** — `RegisterNatives()` 里把创建出来的 `Method` 额外压进一个进程级 keepalive 表。
   注册给原生代码的方法**永不被释放**，缓存的 id 就不可能失效。（`UnregisterNatives()` 仍会从 `methods` 里摘掉它，
   语义不变：之后重新注册会创建新对象并被 find 到。）
2. **`projects/unityloader/javastubs/fakefmod.cpp`** — 不再用 `MethodProxy::invoke`，改为每次调用都重新解析一次、
   并**校验解析到的条目 name/signature 确实是想要的那个**，然后自己按函数指针调。
   （`fmodGetInfo` / `fmodProcess` 都不读 `thiz`，直接传类指针等价。）

### 结果

```
[BD-AUDIO] local_fmodGetInfo(id=0) native=0x3800afb5e4 -> 24000
[BD-AUDIO] local_fmodGetInfo(id=4) native=0x3800afb5e4 -> 2
[BD-AUDIO] SDL Audio device opened. Rate: 24000, Channels: 2
```

无 `SIGSEGV`、无 `[BD-EXIT]`，**跑满整个 25 s 观测窗口**（此前 3.9 s 就退出），
`eglSwapBuffers` **从"一次都没调"变成持续被调**，`frame.1/2/3.gl.ppm` 都写出来了。

### 通用教训

- **`jmethodID` / `jfieldID` 只要可能跨帧缓存，就必须保证对象不被释放。** 这类 bug 的表现是"第一次好、第二次坏"，
  而且坏得像个逻辑错误（走错函数），完全不像野指针 —— 野指针会崩，这个只会安静地调错东西。
- **定位手法**：给可疑的缓存 id 加"每次调用都打印它解析出的 `Method*` 和关键字段"。
  一旦看到**同一个指针两次打印出不同内容**，就不用再查逻辑了，直接查生命周期。
- **`native` 字段"变成了另一个合法地址"是关键指纹**。若内存被写坏，值通常是垃圾；
  这里值恰好是同一个表里相邻的条目 → 指向"对象被复用"，而不是"被覆盖"。

---

## 12. 案例 C（原案例四·续三）：hook 了一个"转发 thunk" —— 已定论

> **本案例与 §0.6-C/D 是同一次取证的两种写法**：完整反汇编、运行时 args 佐证、以及待实施的修法，
> **一律以 §0.6 为准**。这里只留可复用的**动手前检查清单**与通用教训。

### 动手 hook 一个 stripped 的 so 之前：三件必须先做的事

1. **确认这个地址是"函数入口"还是"函数中段"**：
   `objdump -d libunity.so | grep -nE "(bl|b|cbz|tbnz)[[:space:]]+.*<addr>"` —— 有 `bl` 指向它，才可当函数入口 hook。
2. **把它周围的指令读出来**，别只看符号名（stripped 的 so 只有 `JNI_OnUnload+0x…` 这种无意义名）。
   入口序列若是 `ldr x0,[x0,#N]` + `br` 的形状，就是**转发 thunk**，不是本体。
3. **回到调用点看返回值怎么被消费** —— 决定性的一步。
   ```asm
   53f150  bl   4c124c
   53f154  tbz  w0, #0, <失败分支>  ; ★ 返回 bool
   53f158  ldp  x8, x3, [sp, #16]   ; 成功后把两个相邻槽当 {ptr, size} 读出
   ```

### 通用教训

- **hook 成功 ≠ ABI 对上。** 装上了、打印了、触发了一次，只证明"跳转发生了"；
  真正要证的是**返回值/输出参数的约定**。
- **`tbz w0, #0` / `tbnz` 是"测某一位"，不是"测非零"。** 任何"返回指针去满足返回 bool 的接口"的写法都必然为假，
  因为对齐指针低位是 0。看到 `tbz/tbnz w0, #0` 就要立刻想到：这个函数的返回值是**布尔**。
- **判断"结构体是不是按值返回"，看调用前 x8 有没有被设。** 没设、且被 hook 的代码自己拿 x8 当暂存，
  就一定不是 sret。
- **动手 hook 一个 stripped 的 so 之前，先花 10 分钟读三处汇编**：目标地址是不是函数入口、
  它的入口序列是什么形状、**它的调用点怎么消费返回值**。这三处读完，绝大多数"打了 hook 没效果"
  都能在编译前就避免。
- 反过来，`GetMethodID`/`jmethodID` 那类"对象即接口"的坑（见 §11）也一样：
  **盯住数据流的终点**，别只看"代码有没有被执行"。

---

## 13. 相关文档

- `AGENTS.md` —— 日志/构建开关、Dropbeak 推拉文件
- `docs/PORTING_PLAYBOOK.md` —— 端口化通用流程、§1.1 构建命令、§1.2 `JNIVM_ENABLE_RETURN_NON_ZERO`、§1.3 缓存项
- 本文件 §10–§12 —— 案例存档：原 `docs/CASE_STUDIES.md` 的 Oddmar 三节（案例四 / 四·续 / 四·续三）
- `.workbuddy/memory/2026-09-21.md` —— 第二场工作日志
- `.workbuddy/memory/2026-09-22.md` —— 第三～五场工作日志（本文件 §0.6 / §0.7 的原始记录）

> **本文件原名 `docs/HANDOFF-ODDMAR.md`**，2026-09-22 收尾时改名为 `docs/ODDMAR.md`。
> 旧日志、代码注释里出现的 `HANDOFF-ODDMAR.md`，以及简称 "HANDOFF"，指的都是**本文件**。

**版本**：2026-09-22 深夜 · 收尾（第六版 —— **改名 `ODDMAR.md` + 结论汇总 + 任务整理**）。
本版相对第五版：文件改名并在 §13 说明；新增 **§0.3「主要问题：症状 → 根因 → 解法」速查表**
（原 §0.3 顺延为 §0.4）；§7 由「建议的下一步」改为 **「后续任务（按优先级）」**，并增加与 §0.2 的
L# 对应速览表；同步修掉 §7.0 与 §9.2 里残留的旧 `init time` 判据；全仓 4 处代码注释与
`.workbuddy/memory/MEMORY.md` 的文档引用一并改到新文件名。
第五版为 2026-09-22 深夜 · 收尾（L2 定论入库 + 全仓清理记录）；更早为 2026-09-22 晚
（黑屏解决，§0.6-C/D/E + §0.6-H）、2026-09-22 精简版、第三场版、2026-09-21 第二场版。
需要旧版全文：`git log --oneline --follow -- docs/ODDMAR.md`。

**L2 那轮的产物**（容器 `/game/Oddmar/`）：`log-l2probe.txt`（健康轮，含探针输出）、
`log-l2apk.txt`（占位 APK 轮，启动死的对照）、`l2.out` / `l2apk.out`、`seq-l2/`、`seq-l2apk/`。
复现（照抄）：

```bash
# ① 探针 + hook（健康轮）
docker exec GlES_Dev bash -lc '
  cp /game/Oddmar/unity.toml.bak-rd9 /game/Oddmar/unity.toml
  cd /game/Oddmar &&
  GAME_ROOT=/game/Oddmar SEQ_DIR=/game/Oddmar/seq-l2 SECS=60 CAP_MS=500 \
  BOOT_LOADER=/workspace/Bogodroid/build-oddmar-verbose/unityloader \
  BD_BYPASS_VIDEO_TRANSLATE=1 BD_PROBE_APPPATHS=1 \
  bash /run-oddmar-seq.sh > /game/Oddmar/l2.out 2>&1'
grep -a "PATH-PROBE\|BD-L2" /game/Oddmar/log-seq.txt      # dataPath='' + vtbl[0x170]=libunity+0x30c704

# ② 决定性实验：放一个真实普通文件，dataPath 立刻变正常（但启动会死 —— §0.7-A⑧）
docker exec GlES_Dev bash -lc '
  printf "BD-APK-PLACEHOLDER\n" > /game/Oddmar/gamedata/UnityDataAssetPack.apk
  cd /game/Oddmar &&
  GAME_ROOT=/game/Oddmar SEQ_DIR=/game/Oddmar/seq-l2apk SECS=60 CAP_MS=500 \
  BOOT_LOADER=/workspace/Bogodroid/build-oddmar-verbose/unityloader \
  BD_PROBE_APPPATHS=1 bash /run-oddmar-seq.sh > /game/Oddmar/l2apk.out 2>&1
  rm -f /game/Oddmar/gamedata/UnityDataAssetPack.apk'   # ← 记得删掉，否则后续轮次必死
```

反汇编取证用的临时文件（容器 `/tmp/`）—— **收尾时已删，需要时按下述命令重生**
（源：`/game/Oddmar/gamedata/lib/arm64-v8a/libunity.so`，**它是游戏资源，别删**）：

```bash
docker exec GlES_Dev bash -lc 'objdump -d --no-show-raw-insn \
  /game/Oddmar/gamedata/lib/arm64-v8a/libunity.so > /tmp/uni.asm'
docker exec GlES_Dev bash -lc 'readelf -rW \
  /game/Oddmar/gamedata/lib/arm64-v8a/libunity.so > /tmp/relocs.txt'
```

> 该 build 的 **vaddr == file offset**（`.text`/`.rodata` 均是），所以 `strings -t x` 的偏移可直接当虚拟地址用。
> adrp+add 配对扫描的小工具（`xref.py` / `grab.py` / `findg.py`）已随 `/tmp` 清掉，方法见 §0.7-A②。
