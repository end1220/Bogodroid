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
   正经修法见 [`FIVEHEARTS.md`](FIVEHEARTS.md) §4.6 B：**RGBA8 的 `glTexStorage2D`
   不缩**（RT/视频目标常见 `levels==1`）；内容省内存靠离线 ASTC；`glTexImage2D`
   内容上传路径可继续 cap。**截至 2026-09-18 正经修法仍未实现**，上机默认
   `textureMaxDim = 0`。
5. 版本跨度上值得注意：本 case 是 Unity **2022.3**（走 `glTexStorage2D` 的不可变 storage），
   Skul / Maximus2 是 2020.3 —— 老的 `glTexImage2D` 路径 `textureMaxDim` 同样是隐患，
   只是当时没撞上 RT。
6. 片源不要为单机 drawable 降到面板像素：目标机还有 **960×720** 等；默认保持 720p。

## 案例四：Oddmar（Unity 2018.4.36f1 / arm64 / Wwise + Firebase）— **启动即退出（进行中）**

### 当前状态

加载器跑满 30 s 干净退出（`exit=0`），**无任何崩溃**：资源全部加载、
`nativeRender` 首帧正常返回（+2077 ms）、InControl 1.8.6 已进 `active_report`、
Unity Analytics 拿到 `unity.cloud_userid` / `player_sessionid`，PlayerPrefs 落盘 7 条。
随后 `[BD-EXIT] nativeRender returned false - Unity requested quit` ——
**Unity 自己要求退出**，大概率是启动期某个受管异常未被吞掉。首帧仍是纯色（未换帧）。
下一步是找出那个受管异常。

### 最大的一课：`[BD-SEGV]` 的 backtrace 会骗人

上一轮把崩因判成「`libunity+0x319984` 空指针」，**是错的**。原因有两层：

1. **Unity 自己注册 crash handler**。`libunity` 的 `JNI_OnLoad` 会对
   SIGILL/SIGABRT/SIGBUS/SIGFPE/**SIGSEGV**/SIGPIPE/SIGSTKFLT 装自己的 handler
   （`libunity+0x3196bc`），**覆盖** `main()` 里早先装的 `segfault_handler()`。
   它随后会**链式回调**被它替换掉的旧 handler（`libunity+0x319b2c` 把旧 handler 取出来再进），
   所以我们最终还是能拿到信号——但栈已经不属于我们了。
2. **那个 handler 把 TLS 基址放在 `x29`**（`mrs x29, tpidr_el0`），不是 frame pointer。
   glibc 的 `backtrace(3)` 走 frame pointer 链，于是把 TLS 内存当栈帧读，
   吐出一个**落在 Unity 自己 handler 里的假外层帧**。
   `0x319984` 只是 `0x319980: bl 0x319b2c` 的返回地址，不是故障点。

**判据（一眼可辨）**：假帧地址与日志里 `[SIGNALS] handler 0x...` 打印的 handler
地址只差几百字节（本例 `0x319984` 距 `0x3196bc` 仅 0x2C8），且
`[SIGNALS]` 段能看到 Unity 对 signum 11 的 install。

**修法（已实现）**：`print_backtrace_on_segfault()` 改用 `sigaction` + **SA_SIGINFO**
（Unity 的 handler 会把它收到的 `siginfo`/`ucontext` 原样转发过来），
在 handler 里直接读 `uc_mcontext.pc / regs[29] / regs[30] / sp` 与 `si_addr`，
再用 `bd_describe_crash_address()` 解析成 `module+offset`；
栈回溯改用 `bd_dump_crash_backtrace()`（复用 `misc.cpp` 里给 abort 写的
「fp 链 + 保守栈扫描 + 只认 call site 前一条指令」那套），
不再依赖被污染的 frame pointer。

### 真因：pending Java 异常 + 空 `std::exception_ptr`

修好诊断后一次就定位：

```
[BD-SEGV] signal 11 si_code=1 si_addr=0xffffffffffffff80
BD-SEGV:   pc /tmp/bd-unityloader+0x46fd7c     →  addr2line: std::rethrow_exception
```

反汇编该点：`sub x1, x20, #0x80` / `ldaxr w0, [x1]` —— x20（exception object）为 **0**。
libstdc++ 的 `rethrow_exception` 要读 `__cxa_exception` 头（在异常对象**下方 0x80 字节**），
空 `exception_ptr` 就变成读 `0 - 0x80 = 0xffffffffffffff80`。
两处调用点都在 `libjnivm`：`method.h` 的 `j2invoke` 尾部与 `vm.cpp` 的 `ExceptionDescribe`。

链路：Unity 的受管异常路径会 new 一个 `java/lang/Error` 并 `JNIEnv::Throw()` 交给 jnivm
（见 `javastubs/javac.h` 的 `[BD]` 注释，`Error`/`Exception` 就是为它注册的），
但 `Throw()` 只是把 throwable 存进 `env->current_exception`，**没人填它的 `except`**；
下一次 JNI 调用走到 `j2invoke` 尾部就 rethrow 空指针 → SEGV。

**修法（已实现）**：

- `libjnivm/include/jnivm/throwable.h`：新增 `jnivm::RethrowThrowable()`，
  空 `except` 时抛普通 `std::runtime_error` 而不是 `rethrow_exception()`（不变量保护）。
- `libjnivm/include/jnivm/method.h`：`j2invoke` 只在 `except` 非空时 rethrow。
- `libjnivm/src/jnivm/vm.cpp`：`Throw()` 保持 `except` 为空（即"pending"），
  `ExceptionDescribe` 走 `RethrowThrowable()`。

语义依据：**在 Android 上 `Throw()` 只是置起 pending 异常，调用本身正常返回**，
由调用方用 `ExceptionCheck()`/`ExceptionOccurred()` 取回；把异常解栈进游戏的原生帧
（libunity 里没有 try/catch）只会 `std::terminate()`。

### 通用教训

1. **不要相信掌机日志里的裸 backtrace**。工程里已有 `addr2line` 可用的 `-g` 构建；
   先拿 `pc`/`si_addr`，再反汇编，别从 `backtrace_symbols` 的帧序推因果。
2. **`rethrow_exception(空 exception_ptr)` 是致命而非"抛个空异常"**，
   故障现象是 `si_addr=0xffffffffffffff80`（= `-0x80`），与任何游戏代码都无关。
3. **Unity 会抢信号处理器**。若以后要保自己的 SEGV handler，得在 Unity `JNI_OnLoad`
   之后再装一次，或让 `sigaction_impl` 拒收游戏对 SIGSEGV 的替换；
   现在靠 SA_SIGINFO 从链式调用里拿 ucontext 已够用。
4. `open("")` 有专门诊断（`bd_dump_crash_backtrace("open-empty")`）。
   Oddmar 有 14 次，全来自 `libunity+0x8a2b58`，是良性探测（拿 -1 继续走），
   别当成崩因。

### 黑屏截图 ≠ 渲染成功：先证明 `eglSwapBuffers` 被调过

Oddmar 修复 SEGV 后的第一次"成功"运行（exit=0）给出的是 640×480 纯黑截图。
**这张图是真的黑，不是抓图时机问题**，但也不能据此说"渲染坏了"——要先把两件事分开：

`run-oddmar.sh` 的截图点在 `SECS-2`。SECS=30 时它等到 28 s 才 `import -window root`，
而 Unity 在 3.9 s 就 `nativeRender -> false` 退出了；**窗口早没了，抓到的是空的 root window**。
这层解释只说明"这张图没意义"，不是"画面是黑的"。

要看真东西，用 `_harness/run-oddmar-seq.sh`：进程存活期间每 250 ms 抓一张，
同时 `BD_DUMP_FRAME` 让 loader 自己 `glReadPixels` GL drawable（`BD_DUMP_FRAME_AT=1`
→ swap #1/#2/#3 各出一张 `.gl.ppm`）。判据：

| 观察 | 结论 |
|------|------|
| 有 `.gl.ppm`，且 root 截图黑 | 画了但没 present（present 通道问题） |
| 没有 `.gl.ppm` | `eglSwapBuffers` **一次都没被调用**，Unity 根本没画完一帧 |
| 两者都有内容 | 渲染+present 正常，问题在别处 |

Oddmar 实测（修复版，`BOOT_LOADER=build-oddmar-verbose/unityloader`）：

```
帧序列 t=23/575/1114/1763/2294/2827/3357 ms   每张 192 B（1-bit 640×480 全 0，纯黑）
窗口探测  windows=0 直到 ~1.6 s，之后 windows=1 持续到退出
swap      eglSwapBuffers 调用次数 = 0，无任何 .gl.ppm
退出      exit=0 @ 3.9 s  （[BD-EXIT] nativeRender returned false）
```

即：窗口建起来了、GL 3.2 context 也 `makeCurrent` 过，但 **Unity 两次 `nativeRender`
都没走到 present**，第二次直接返回 false 退出。所以"最终画面"就是纯黑——因为
**从来没有过一帧**，不是画黑的。192 B 这个体积本身就是判据：640×480 单色图压完就这么大，
几十 kB 才说明有内容。

**两个容易踩的点**：

1. **`BOOT_LOADER` 有默认值**，指向 `/workspace/Bogodroid/build-oddmar/unityloader`。
   只重建了 `build-oddmar-verbose` 而忘了传 `BOOT_LOADER` 时，跑的是修好之前的老二进制，
   会看到"崩溃又回来了"的假象（`[BD-SEGV]` 还是旧格式 `backtrace_symbols_fd` 的裸地址）。
   判断方法：新 handler 打 `si_code`/`si_addr`/`pc`/`lr`，旧的只打一行 `[BD-SEGV] signal N`。
2. **`xwininfo -root -children | grep -c '^     0x'` 数的就是子窗口数**，
   0 → 还没有 X 窗口（SDL 窗口是在第一次 `nativeRender` 内部创建的），
   用来把"窗口没建"和"窗口建了但没内容"分开。

---

## 案例四（续）：**悬垂的 `jmethodID`** —— 缓存的方法 id 悄悄换了个函数

### 症状

补完 `StackTraceElement` / `setStackTrace` 桩之后，Unity 拿到了正确的 FMOD 音频参数，
但进程仍然 `SIGSEGV`。崩点非常有规律：

```
[BD-DBUF]: GetDirectBufferAddress handle=(nil)  from 0x3800afb760
[BD-DBUF]: GetDirectBufferAddress handle=0x4    from 0x3800afb760   ← 崩在这
```

`0x3800afb760` = `libunity + 0xafb760`。调用方是
`FMODAudioDevice::local_fmodGetInfo(4)`：它取 `fmodGetInfo` 的地址去调，`4`（`infoId`）
被当成了 `ByteBuffer` 句柄传进 `GetDirectBufferAddress`。

### 排除法（这几步都不能省）

1. **表本身没串号**。按 24 字节步长扫 libunity 的 `JNINativeMethod` 三元组
   （`readelf -rW` 取 `R_AARCH64_RELATIVE` 的 addend，锚定 `name` 串后看 +8/+16）：

   | 表项偏移 | name | signature | fnPtr |
   |---|---|---|---|
   | `0xe9d008` | `fmodGetInfo` | `(I)I` | `0xafb5e4` |
   | `0xe9d020` | `fmodProcess` | `(Ljava/nio/ByteBuffer;)I` | `0xafb6ac` |
   | `0xe9d038` | `fmodProcessMicData` | `(Ljava/nio/ByteBuffer;I)I` | `0xafb738` |

   全文件 42 条 native 表项，`fmod*` 只有这三条，**没有重复注册**。
2. **`fmodGetInfo` 不可能调 JNI**。反汇编 `0xafb5e4`：`mov w19, w2` 存下 `infoId`，
   然后 `cmp w19, #4 / br` 跳表；跳表 `0xcb871c` 的 5 个 32 位相对偏移分别指向
   `0xafb678 / afb694 / afb69c / afb684 / afb6a4`，全是"返回一个 int"。整个函数
   **一条 `blr` 都没有**。
3. **0xafb760 确实属于 `fmodProcessMicData`**（`0xafb738` 起，`blr x8` 在 `0xafb75c`）
   ——它才是那个 `GetDirectBufferAddress(env, x2)` 的调用者。
4. **jnivm 侧的注册是对的**。运行时打印 `org/fmod/FMODAudioDevice` 的 `methods`：
   `fmodGetInfo → 0x3800afb5e4`、`fmodProcess → 0xafb6ac`、`fmodProcessMicData → 0xafb738`，
   与文件表完全一致。`getMethod("(I)I","fmodGetInfo")` 解析出的 `Method` 也打印了
   `native=0x3800afb5e4`。

### 真因

在 `local_fmodGetInfo` 里**每次调用都打印缓存的 `Method*` 和它的 `native`**，两次
`runAudio` 之间出现了这一幕：

```
1128: local_fmodGetInfo(id=0) mid=0x4001e15da420 native=0x3800afb5e4   ← 第一次正常
1317: [FMODAudioDevice] Exiting audio loop.
1320: local_fmodGetInfo(id=0) mid=0x4001e15da420 native=0x3800afb738   ← 同一个指针，值变了
1322: local_fmodGetInfo(id=4) mid=0x4001e15da420 native=0x3800afb738   ← 于是崩
```

**同一个 `Method` 对象的 `native` 字段换了值**，而且换成了一个合法的、
就在表里紧挨着的函数地址。这不是内存被写花，是**对象被释放后同尺寸分配复用了**：

- jnivm 的 `jmethodID` 就是裸的 `Method*`；
- `RegisterNatives()` 把每个原生方法 `make_shared<Method>()` 后 `push_back` 进
  `Class::methods`（`vm.cpp`）；
- `UnregisterNatives()` 会把 `native != nullptr` 的方法从 `methods` 里 `erase`
  → 最后一个 `shared_ptr` 掉了 → **方法对象被 free**；
- 下一次同尺寸的 `Method` 分配（重新注册、或任何 auto-stub 方法）**复用了同一块内存**，
  于是那个缓存的 id 还指向有效地址，却安静地变成了另一个 native 函数。

`FakeJni::MethodProxy` 正是那种"解析一次、缓存 `jmethodID`"的调用者，
`local_fmodGetInfo` / `local_fmodProcess` 里的 `static auto` 也是。
第一次 `runAudio` 里 `fmodGetInfo` 还是好的（所以音频参数都对），
第二次 `runAudio`（Unity 停了又起一次音频设备）时就已经是 `fmodProcessMicData` 了。

### 修复

1. **`libjnivm/src/jnivm/vm.cpp`** — `RegisterNatives()` 里把创建出来的 `Method`
   额外压进一个进程级 keepalive 表。注册过给原生代码的方法**永不被释放**，
   缓存的 id 就不可能失效。（`UnregisterNatives()` 仍然会从 `methods` 里摘掉它，
   语义不变：之后重新注册会创建新对象并被 find 到。）
2. **`projects/unityloader/javastubs/fakefmod.cpp`** — 不再用 `MethodProxy::invoke`，
   改为每次调用都重新解析一次、并**校验解析到的条目 name/signature 确实是想要的那个**，
   然后自己按函数指针调。多一层"这不是我要的方法"的拒绝，比直接崩掉好得多。
   （`fmodGetInfo` / `fmodProcess` 都不读 `thiz` 参数，所以直接传类指针等价。）

### 结果

```
[BD-AUDIO] local_fmodGetInfo(id=0) native=0x3800afb5e4 -> 24000
[BD-AUDIO] local_fmodGetInfo(id=4) native=0x3800afb5e4 -> 2
[BD-AUDIO] local_fmodGetInfo(id=1) native=0x3800afb5e4 -> 1024
[BD-AUDIO] local_fmodGetInfo(id=2) native=0x3800afb5e4 -> 4
[BD-AUDIO] SDL Audio device opened. Rate: 24000, Channels: 2
```

无 `SIGSEGV`，无 `[BD-EXIT]`，**跑满整个 25 s 观测窗口**（此前 3.9 s 就退出），
而且 `eglSwapBuffers` **从"一次都没调"变成持续被调**，`frame.1/2/3.gl.ppm` 都写出来了。

### 通用教训

- **`jmethodID` / `jfieldID` 只要可能跨帧缓存，就必须保证对象不被释放。**
  这类 bug 的表现是"第一次好、第二次坏"，而且坏得像个逻辑错误（走错函数），
  完全不像野指针——野指针会崩，这个只会安静地调错东西。
- **定位手法**：给可疑的缓存 id 加"每次调用都打印它解析出的 `Method*` 和关键字段"。
  一旦看到**同一个指针两次打印出不同内容**，就不用再查逻辑了，直接查生命周期。
- **`native` 字段"变成了另一个合法地址"是关键指纹**。如果是内存被写坏，
  值通常是垃圾；这里值恰好是同一个表里相邻的条目，指向"对象被复用"，而不是"被覆盖"。


