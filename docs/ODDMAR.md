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

> **最后一次更新：2026-09-22 深夜 · 收尾**（分支 `oddmar`；`ddmar` 是 09-22 目录/分支重排前的旧名）
> —— **黑屏已解决（§0.6-H）**，**L2 的 hostless URL host 来源也已定位（§0.7）**：
> host = `Application.dataPath`，被 Unity 的 `stat()` + `S_IFREG` 闸门挡在门外。
> 文档与代码本轮已一并收尾提交（见 §13 版本行）。

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
| `eglSwapBuffers` | 帧循环在跑（`Choreographer$FrameCallback.doFrame` ≈21 Hz） |
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
| Wwise 起不来 | `WwiseUnity: Failed to initialize the sound engine. Reason: AK_Fail`，且 `AkInitializer.cs Awake() was not executed yet` 每帧刷 | 不崩，优先级最低 |
| `[JNIVM] Invalid Reference, Unexpected Type` | **✅ 已结案**：只在**哨兵配置**下出现，与 `JNIVM_ENABLE_RETURN_NON_ZERO` 无关（§0.6-F 第 2 条） |

---

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

- `build-oddmar-verbose/` = 本次迭代用的构建目录（`CMAKE_BUILD_TYPE=RelWithDebInfo`，`JNIVM_ENABLE_RETURN_NON_ZERO=OFF`）
- `build-oddmar/` = **旧产物，7:55 的二进制，体积只有 6MB**（`build-oddmar-verbose` 是 150MB）
- ⚠️ **`JNIVM_ENABLE_RETURN_NON_ZERO` 是 CMake 缓存项**，共用构建目录时务必确认 `grep JNIVM_ENABLE_RETURN_NON_ZERO build-oddmar-verbose/CMakeCache.txt` 为 `OFF`

### 1.3 运行（逐帧截图 + loader 侧 glReadPixels）

```bash
docker exec GlES_Dev bash -c 'rm -rf /game/Oddmar/seqN /game/Oddmar/log-seq.txt; \
  mkdir -p /game/Oddmar/seqN; cd /game/Oddmar && \
  BOOT_LOADER=/workspace/Bogodroid/build-oddmar-verbose/unityloader \
  SEQ_DIR=/game/Oddmar/seqN SECS=32 BD_ENABLE_LOG=1 bash /run-oddmar-seq.sh \
  > /game/Oddmar/seqN/run.out 2>&1; echo "EXIT=$?"'
```

产出：
- `/game/Oddmar/log-seq.txt` —— loader 的 stderr（**主日志**）
- `SEQ_DIR/0000.png…` —— X root window 截图（`import -window root`）
- `SEQ_DIR/frame.{1,2,3}.gl.ppm` —— loader 自己 `glReadPixels` 的 drawable（由 `BD_DUMP_FRAME`/`BD_DUMP_FRAME_AT` 触发）

### 1.4 ⚠️ 三个必踩的坑

1. **`BOOT_LOADER` 默认指向老二进制**
   `/run-oddmar-seq.sh` 第 17 行：`BOOT_LOADER="${BOOT_LOADER:-/workspace/Bogodroid/build-oddmar/unityloader}"`。
   **每次都要显式传 `BOOT_LOADER=.../build-oddmar-verbose/unityloader`**，否则你在跑 7:55 的旧产物，所有改动都不生效。
2. **bash 环境缺 PATH**
   每条命令前加：
   `export PATH="/usr/bin:/bin:/c/Windows/System32:/c/Users/Administrator/.workbuddy/binaries/PortableGit/versions/1.2.0/usr/bin:$PATH"`
   （否则 `dirname: command not found`，docker 相关命令可能拿不到输出。）
3. **`docker cp` 用绝对 Windows 路径**，且 `javastubs/` 有**两个**目录：
   - 仓库根 `javastubs/`（`android*.cpp`、`javac.cpp`、`bd_assetlocator.cpp` …）
   - `projects/unityloader/javastubs/`（`unity.cpp`、`fakefmod.cpp`、`binding.cpp` …）
   两边都在 `-I` 里，**别 cp 错目录**。

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
