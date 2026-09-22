# Oddmar 移植接手文档（video 分支）

> 写于 2026-09-21。交接给下一个会话：**先读完本文再动手**。
>
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
> **2026-09-21 更新（P0 已修）**：`AssetLocator.<init>` 的黑洞不是 `GetMethodID` 的查找/改写，
> 而是 `ReflectionHelper.getConstructorID()` 交出去的那个 `reflect::Constructor` **从来没有执行体**。
> 已在 `projects/unityloader/javastubs/unity.cpp` 里把它绑定到真正注册的 `<init>` 条目，实测
> `AssetLocator` 构造成功、`ListAssets('SoundBanks') -> 153 entries`，本文件 §4.4 原结论已作废（见 §6 P0）。
> **画面仍全黑**，卡点已离开资源定位：8 个 AssetBundle 已真实加载、进程跑满 120 s 不崩。
> 另有一个**高优先级的回归风险**：本次对 `libjnivm` 的改动是全局行为改动，**必须先回归其他游戏**
> （§7.1 第 1 条已用 Samurai2/Maximus2 跑过一轮 4 组对照，全通过）。

---

## ⛔ 接手第 0 步（不看这四条会白跑一轮）

> 以下四条都是**当前环境的真实状态**，不是建议。全部核对于 2026-09-21 晚·第二场结束。

**0-1. 容器里的 `unity.toml` 现在还是"哨兵"状态，必须先还原**

`/game/Oddmar/unity.toml` 目前被上一轮实验改坏，**会直接打断启动**（`init time` 与 8 个 bundle 全部消失）。
还原（备份就在旁边）：

```bash
docker exec GlES_Dev bash -lc 'cp /game/Oddmar/unity.toml.bak-rd9 /game/Oddmar/unity.toml && \
  grep -n "android_package_code\|android_source_dirs\|android_obb_dirs" /game/Oddmar/unity.toml'
```

期望输出：`android_package_code="./"`、`android_source_dirs=["./"]`、`android_obb_dirs=["./"]`。
哨兵版与备份版的差异**只有两行**（`diff` 实测）：`android_package_code` 被改成 `/tmp/ZZZSENTINELPACK.apk`，
`android_source_dirs` 被清空成 `[]`。

**0-2. `BOOT_LOADER` 默认指向旧产物**，每次运行都必须显式传（见 §1.4 第 1 条）。
本轮用的是 `/workspace/Bogodroid/build-regress/unityloader`（RelWithDebInfo，14:31）。

**0-3. 宿主机 `projects/unityloader/main.cpp` 里有一份"半成品探针"，容器里没有**

host md5 `b4e0ba39769703b3168347ee608317c8` ≠ container md5 `1dad86c24186eaf3daab5219c1b922d4`
（container 版本停在 11:29，**不含探针**）。探针能编过但**完全没接线**（详见 §0.5-D）。
⚠️ 谁要 `docker cp` 这份 main.cpp，就会把死探针一起带进构建。

**0-4. 容器内 `seq*` 临时目录已堆到 50 个、日志备份 7 个**，新会话开工前建议先清（§9 有清单）。

---

## 0.5 第二场（2026-09-21 晚）做了什么 —— 含两处**结论更正**

### A. ✅ NDK 视频符号链已补齐（实测生效）

补齐前，Unity 明确放弃 NDK 路径：

```
AndroidMediaNDK could not load symbol AMediaFormat_toString, will stop loading NDK.
AndroidMediaNDK: libmediandk.so not loaded but expected to be present in API level 29 (>=21).
       NDK-based video playback disabled, will use JNI instead.
```

补法是**逐个试错**——每补一个符号，报错就换成下一个：

```
AMediaFormat_toString            ← 第 1 轮
  ↓ 补
AMediaExtractor_unselectTrack    ← 第 2 轮
  ↓ 补
AMEDIAFORMAT_KEY_AAC_PROFILE     ← 第 3 轮
  ↓ 补齐 AMediaExtractor_getSampleFlags / AMediaCodec_* / AMEDIAFORMAT_KEY_* 全集
（"could not load symbol" 消失）
```

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

### B. ⚠️ 更正：上一轮的"哨兵 A/B 决定性实验"**结论不成立**，必须重做

上一轮记录的结论是：

> "把 `android_package_code` 改成 `/tmp/ZZZSENTINELPACK.apk` 后，`jar:file://` 这条错误**完全消失**了。
> 说明 dataPath 确实来自 `sourceDir`/`getPackageCodePath`。"

**本轮核对推翻它。** 两个理由：

1. **那次实验同时改了两个变量**（`diff` 实测只差两行）：
   | | 备份（正常） | 哨兵 |
   |---|---|---|
   | `android_package_code` | `"./"` | `"/tmp/ZZZSENTINELPACK.apk"` |
   | `android_source_dirs` | `["./"]` | **`[]`** |
2. **清空 `android_source_dirs` 直接打断启动**。rd9b 实测：`[BD-ASSETLOC]` 出现 **0 次**、
   bundle 相关行 **0 次**、`init time` **0 次**；日志最后一条 `[BD-TIME]` 停在
   **`+370 ms before first nativeRender`**（line 255），全文**只有 512 行**（正常轮次 3 900+ 行）。

→ 所以 `jar:file://` 错误"消失"**只是因为进程卡死得更早、压根没跑到视频那一步**，
不是因为 URL 被拼正确了。**这个实验什么都没证明。**

**正确做法**：重做 A/B 时**只改 `android_package_code`**，`android_source_dirs=["./"]` 必须保留。

### C. 🔴 更正后的**真嫌疑**：`getObbDir()` / `getObbDirs()` 是 STUB-MISS 返回 null

在 rd9b 日志里，紧挨着 `getPackageCodePath` 之后（line 337）就是 obb 查询：

```
299: [BD-DATADIR] PackageManager.getApplicationInfo(com.mobge.Oddmar, 0x80)
302: [BD-DATADIR] ApplicationInfo.sourceDir = /tmp/ZZZSENTINELPACK.apk
337: [BD-DATADIR] getPackageCodePath -> /tmp/ZZZSENTINELPACK.apk      ← 配置值确实传出去了
344: [BD-JNIVM]: [BD-ANY-MISS] cls=android/content/Context str0='getObbDirs' str1='()[Ljava/io/File;'
345: [JNIVM]: [STUB-MISS] MethodID: Class=`android/content/Context` Member=`getObbDirs` -> returning default
387: [JNIVM]: [STUB-MISS] Unknown Member: Class=`android/app/Activity` Member=`getObbDirs` -> returning default
388: [BD-JNIVM]: [BD-ANY-MISS] cls=android/content/Context str0='getObbDir' str1='()Ljava/io/File;'
389: [JNIVM]: [STUB-MISS] MethodID: ... Member=`getObbDir` -> returning default
433: [JNIVM]: [STUB-MISS] Unknown Member: Class=`android/app/Activity` Member=`getObbDir` -> returning default
```

**为什么这条直指 `jar:file://!/assets/...` 的空 host**：Unity 在 Android 上把 StreamingAssets
拼成 `"jar:file://" + <obb 路径 或 apk 路径> + "!/assets"`。**obb 路径为 null ⇒ host 为空 ⇒ `jar:file://!/assets/...`。**
而空 host 正是 §6 P0-b 那个 `-10004` 直接成因。

现状：这个洞是**当初有意留的**，不是意外——

- `javastubs/android.h:979` 注释：`// getContentResolver / getObbDir / getObbDirs -> STUB-MISS path`
- `javastubs/android_content.cpp:789`：`// getObbDir / getObbDirs return null — left to the STUB-MISS path`
- `javastubs/android_content.cpp:896`：同一句重复
- 全仓 `grep -rn "ObbDir|obb_dir|obbDirs"` **只有 4 处命中，全是注释，零实现**
- 配置里的 `android_obb_dirs=["./"]` **没有任何代码消费它**

→ **下一步最高优先级**：实现 `Context.getObbDir()` / `getObbDirs()`（返回 `File(android_obb_dirs[0])`，
`java/io/File` 应已有实现可复用）以及 `Activity.getObbDir(s)`，让 URL 的 host 非空。
这条**能同时在干净配置下验证**，与 §0.5-B 的重做 A/B 可以合成一轮。

### D. ⚠️ 宿主机上的 il2cpp 探针是**半成品**，别直接信它能用

上一轮在宿主机 `projects/unityloader/main.cpp` 里加了 `il2cpp_patch::probe_app_paths()`（tag `PATH-PROBE`），
思路是**绕过一切猜测**，直接用 `il2cpp_runtime_invoke` 调托管侧
`Application.get_dataPath / get_streamingAssetsPath / get_persistentDataPath / get_temporaryCachePath`，
把 Unity 自己眼里的值打出来。思路对，但**代码没接线**，四处断点：

1. `g_probe_app_paths`（`main.cpp:181`）**恒为 `false`** —— 没人从环境变量赋值；
2. `init()`（`main.cpp:409`）里**没有**解析 `il2cpp_runtime_invoke`（只解析了 domain/assemblies/image/class/class_get_method_from_name）；
3. `init()` 开头 `if (g_n == 0 && g_post_init_n == 0) return;` → 无 patch 时**直接返回**，探针永不 arm；
4. `probe_app_paths()`（`main.cpp:316`）定义后**无人调用**。

容器侧实测未受影响：`grep -c "PATH-PROBE" build-regress/unityloader` = **0**，
且 container 的 main.cpp 停在 11:29 版本（不含探针）。

**要启用探针需要改的四处**（都在这一个文件里）：

- `g_probe_app_paths = std::getenv("BD_PROBE_APPPATHS") != nullptr;`（建议放在 `init()` 入口）
- `init()` 里补 `il2cpp_runtime_invoke = (p_runtime_invoke)so_symbol(lil2cpp, "il2cpp_runtime_invoke");`
- 早退条件改成 `if (g_n == 0 && g_post_init_n == 0 && !g_probe_app_paths) return;`
- `il2cpp_init_hook()` 里 `install_once();` 之后加 `probe_app_paths("post-init");`

> 探针价值：如果 **`dataPath` 打印出来就是空/非法**，说明根因在 Java 侧（`sourceDir`/`obb`）；
> 如果 **打印出来是正确路径**，那就坐实"Unity 拼 URL 用的不是这两个属性"，直接转去查别处，省一轮。

### E. 本轮实测数字

| 轮次 | 配置 | exit | X 截图帧数 | 截图大小 | `init time` | 8 bundle |
|---|---|---|---|---|---|---|
| rd5–rd8 | 正常 | 124（跑满） | 210–213 | **全 192 B** | 有 | ✅ 已加载 |
| rd9 | 哨兵 45s | 124 | 88 | 全 192 B | **0** | **0** |
| rd9b | 哨兵 120s | 124 | 227 | 全 192 B | **0** | **0** |

- 全部无 `[BD-SEGV]`、无 `terminate`。
- **192 B = 纯色**（`import -window root` 截的 X 根窗口，`docs/CASE_STUDIES.md` 里 180 KB+ 才算有画面）。
- 哨兵两轮的 `seq-rd9*/` 里 **`frame.*.gl.ppm` 一个都没有**（`eglSwapBuffers` 未触发 dump）。

### F. 同期发现、但**不属于视频线**的阻塞

| 现象 | 事实核对 | 处理建议 |
|------|---------|---------|
| 云服务回调不触发 | `SocialImpl.authenticate` / `SaveGames.isConnected` 是 `[STUB-MISS]`、`entries=0`（类未注册）→ 回调查不到。**不是轮询卡死**（各仅 1 次 / 12 次调用） | 断网约束下只需"不阻塞"。若证实它卡住 `MGLOProgressData.construct` 链再修 |
| Wwise 起不来 | `WwiseUnity: Failed to initialize the sound engine. Reason: AK_Fail`，且 `AkInitializer.cs Awake() was not executed yet` 每帧刷 | 不崩，优先级最低 |
| `[JNIVM] Invalid Reference, Unexpected Type` | rd9b 出现 2 次（line 507/509）。**症状与 `JNIVM_ENABLE_RETURN_NON_ZERO=ON` 的经典崩溃一模一样**，但 `build-regress/CMakeCache.txt` 里明确是 `OFF` | ⚠️ 需在**干净配置**下属复核：若只在哨兵轮出现，就是哨兵连带症状 |

---

## 0. 当前状态速览

> **最后一次更新：2026-09-21 晚·第二场**。目标约束见文首：**只需断网单机可跑**，
> billing / Firebase / Google Play / 云存档类缺失一律可绕过，只要不阻塞启动、不致命。

| 项目 | 状态 |
|------|------|
| 进程存活 | **120 s 跑满**，无 `[BD-ANDROID] CRASH`、无 `[BD-SEGV]`、无 `terminate`、无 `NullReferenceException` |
| 启动链 | ✅ 越过云/计费阻塞 → `init time: 0` → **8 个 AssetBundle 真实加载**（`Unable to read header` = **0 条**） |
| NDK 视频通路 | ✅ **已启用**（`AndroidMediaNDK could not load symbol` 已消失，`[BD-MEDIA] extractor new` 被调用） |
| `frame.*.gl.ppm` | 640×480，**307200 像素全 `00 00 00`**（纯黑，非近黑） |
| X 根窗口截图 | 210–227 帧，**每帧恒 192 B = 纯色**（有画面时应 ≥180 KB） |
| `eglSwapBuffers` | 帧循环在跑（`Choreographer$FrameCallback.doFrame` ≈21 Hz） |
| 游戏是否 quit | 未走 Unity 正常 Quit（末尾 `Caught signal, fast-exiting via _exit` 是 `timeout -s INT` 到点） |
| **主阻塞（新定位）** | `Context.getObbDir()` / `getObbDirs()` **STUB-MISS 返回 null** → StreamingAssets URL 的 host 为空 → `jar:file://!/assets/Videos/mobge_and_senri_splash_video.mp4` → `-10004`（§0.5-C） |
| **次阻塞** | 云服务 `SocialImpl.authenticate` / `SaveGames.isConnected` 类未注册 → 回调不触发（§0.5-F） |

**一句话**：黑屏已推到最后一层——**引擎本身健康**（跑满 120 s、8 个 bundle 都读进来了、NDK 视频通路已启用），
唯一挡住画面的是一条**路径拼装缺口**：obb 目录查询返回 null，导致片头视频 URL 的 host 为空、解不成本地文件。
另需注意：上一轮那个"证明 dataPath 来自 `getPackageCodePath` 的哨兵实验"**是无效实验，结论已作废**（§0.5-B），
它把另一个改动（清空 `android_source_dirs`）混了进去，导致进程根本没跑到视频那一步。

### 历史修复（都已实测推进，保留备查）

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

真实设备上没问题（Java 侧解析后喂 `Class.forName`，要的就是点号）；但 jnivm 把字符串**原样**当查找键，而本仓库所有类都以斜杠形式注册 → 永不匹配。
→ 已在两处做归一化：`method.cpp` 的 `normalize_jni_sig()`（`L...;` 内 `.` → `/`）与 `javastubs/javac.cpp` 的 `bd_normalize_jni_sig()`（`Constructor` 构造时）。

---

## 4. 本次改动清单

### 4.1 建议保留（有实测收益）

| 文件 | 改动 | 依据 / 收益 |
|------|------|-------------|
| `libjnivm/src/jnivm/vm.cpp` | `RegisterNatives` 增进程级 keepalive 表（`bd_registered_method_keepalive()`），pin 住注册的 `Method` | 修"悬垂 jmethodID"：`UnregisterNatives` 释放后，同尺寸 `Method` 复用同一块内存，缓存的 id 会安静地变成一个**合法但错误**的函数。见 `docs/CASE_STUDIES.md` 案例四续 |
| `libjnivm/src/jnivm/internal/method.cpp` | ① `<init>` 改写**去掉 `!isStatic` 守卫**；② 签名点号→斜杠归一化；③ **修 else 绑定 bug**（见 4.3） | ①Unity 的 `AndroidJNIHelper.GetConstructorID` 会走 static 重载，旧守卫使改写被跳过；③ 修复后 `DisplayMetrics`/`Handler` 等构造器首次能真正命中 |
| `javastubs/javac.cpp` | `Constructor` 构造时归一化签名 | 堵住 Unity 传来的点号签名 |
| `javastubs/bd_assetlocator.cpp`（新增） | `com.mobge.assetlocator.AssetLocator` 的 C++ 桩：注册 **4 种首参**的构造器（`JObject` / `Context` / `Activity` / `UnityPlayerActivity` + `String`）+ `ListAssets` | 游戏按 `UnityPlayerActivity` 首参查；`ListAssets` 按 `AssetManager.list` 语义返回**子项名** |
| `javastubs/android.h` / `android_content.cpp` / `android_descriptors.cpp` | 新增 `AssetLocator` 声明与注册；`Context::getApplicationContext()`；`AssetManager::list` 的 catch 分支补 `return` | 后者原本控制流落空（UB） |
| `projects/unityloader/javastubs/unity.cpp` | `ReflectionHelper::getMethodID` 的 **Method 1b（继承链 BFS 查找）** | 修 `getIntent`/`getAssets`/`getPackageManager` 等在**基类**上、却用泛型签名查导致返回 `(nil)` 的问题。⚠️ **该改动已在 HEAD 中，不属于 09-21 未提交批次**（审核时确认） |
| `projects/unityloader/javastubs/unity.cpp` | **`ReflectionHelper::getConstructorID` 给返回的 `Constructor` 绑定执行体**（按参数列表在 `clazz->methods` 找注册的 `<init>`，拷 `nativehandle`/`native`，置 `_static=true`） | **P0 修法**。Unity 把这个返回对象直接当 jmethodID 用（static `CallMethod`），而它原本恒空 → 一切经它构造的对象都是 null。见 §4.4 / §6 P0。**不涉 `libjnivm`，无全局回归** |
| `javastubs/android_view.cpp` | `Display::getRealMetrics` 对 `nullptr` 参数做保护 | 修 `SIGSEGV at fault addr 0x58`（`metrics->widthPixels` 空解引用），该崩溃曾让进程在 `nativeRender` 后 2.6s 死掉 |

### 4.2 应清理（纯诊断，验证完就删）

| 位置 | 内容 |
|------|------|
| `libjnivm/src/jnivm/internal/method.cpp` | `[BD-CTOR-ENTER]`（`GetMethodID` 入口，只对 `<init>`）、`[BD-ANY-MISS]` + 注册表 dump（`[BD-MISS]`，在 miss 分支，会遍历该类全部 entries） |
| `libjnivm/src/jnivm/vm.cpp` | `[BD-REG]` 两条（已把 tag 修正为 `BD-REG`）。**注意**：找到问题后建议把这行降级或删除，它在每次 `RegisterNatives` 都打印 |
| `javastubs/javac.cpp` | `Constructor::newInstance` 里的 `lookup ->` / `hit:` / entries dump（**注意 `newInstance` 实际从未被调用**，见 4.4） |
| `projects/unityloader/javastubs/unity.cpp` | `getConstructorID` 里的注册表 dump（`getConstructorID: %s prefix=...` + 每条 entry） |
| `javastubs/android_view.cpp` | `getRealMetrics` 里的 DisplayMetrics dump（**null 保护本身要留**，dump 可删） |
| `libjnivm/include/jnivm/method.h` | `BD_J2_TRACE` 开关（`j2invoke` 里打印 native 指针与签名）。诊断 fmod 悬垂 id 时加的，可留可删 |

### 4.3 ⚠️ 本次自己引入并已修复的 bug（值得记住的教训）

把 `if(!isStatic && sname == "<init>")` 改成 `if(sname == "<init>")` 后，我在**函数体内部**又加了一层 `if (isVoidCtor)`：

```cpp
// ❌ 错误版本
if(sname == "<init>") {
    const bool isVoidCtor = ...;
    if (isVoidCtor) { ...改写...; return ...; }
}
else { /* 查找 */ }
```

当签名**已经是改写形态**（即 static 重入那次）时：`sname == "<init>"` 成立 → 进入 `if` → `isVoidCtor` 为 false → **既不改写也不 return** → **`else` 的查找代码被整个跳过** → 直接落进 miss 分支。

**症状极具迷惑性**：`[BD-MISS]` 打印出的查找键与注册项**逐字节相同**，却判定 miss：

```
[BD-MISS] android/util/DisplayMetrics str0='<init>' str1='()Landroid/util/DisplayMetrics;' static=1 allowNative=0 entries=1
[BD-MISS]   name='<init>' sig='()Landroid/util/DisplayMetrics;' static=1 native=(nil) handle=1
```

**修法**：两个条件一起决定 `if` / `else`：

```cpp
const auto close = ssig.find(')');
const bool bdVoidCtor = close != std::string::npos &&
                        close + 1 < ssig.size() && ssig[close + 1] == 'V';
if(sname == "<init>" && bdVoidCtor) { ...改写...; return ...; }
else { /* 查找 */ }
```

**教训**：往一个"条件 + else"结构里塞内层判断时，务必确认内层判断为假时控制流去哪。

### 4.4 一个反直觉事实：`Constructor::newInstance` 从未被调用

`ReflectionHelper.getConstructorID(clazz, sig)` 返回的 `jnivm::java::lang::reflect::Constructor` 对象，Unity **不会**调它的 `newInstance`（加了入口日志后确认 0 次调用）。
Unity 拿这个 jmethodID 直接去调（`NewObject` 类路径），走了 `method.cpp` 的 **static** `CallMethod` 分支（`[STUB-MISS] Unknown Static` 即出自这里，不是查找阶段）。

> ⚠️ **本文档原版在这里得出的结论是错的，已被实测推翻，保留于此作为教训。**
>
> 原文写的是："所以**在 `getConstructorID` 的返回值上做文章没用**，真正要修的是 `GetMethodID` 的查找/改写（已做）。"
>
> **错在哪里**：把"Unity 不调 `newInstance`"错误地推成了"这个返回对象不重要"。恰恰相反——
> Unity 把这个对象**当 jmethodID 直接用**，所以它本身就承担着执行体的职责。
> 用探针把 `mid` 指针与 `getConstructorID` 的返回值对齐后（`[BD-MID] mid=0x40030d3425f0` 与
> `getConstructorID(...) = 0x40030d3425f0` 完全一致），事实是：
>
> - `javac.cpp` 的 `Constructor` 构造时**只填 `name`/`signature`**，`nativehandle`/`native`/`dynamic` 恒为空；
> - 而 static `CallMethod`（`method.cpp`）只有两条成功路径：`mid->dynamic` 或 `mid->nativehandle`；
> - 两空 → 落进 `Unknown Static` → 返回默认值（null）。
>
> 也就是说：**不管 `GetMethodID` 的改写多完美，都不可能救到这条路**——这个对象压根不是 `GetMethodID` 创建的
> （全轮日志 `[BD-CTOR-ENTER]` 为 0 即证）。签名归一化（§3.3）本身是对的、也在生效，只是**不是根因**。
> 表面症状（点号 + `V`、`handle=(nil)`）全部由"包装对象没绑执行体"解释，不需要额外假设。
>
> **教训**：判断"某个改动有没有用"时，要盯住**数据流的终点**，不能只看"哪条代码路径没被执行"。
> `newInstance` 没被调用 ≠ 这个对象没用；它是"jmethodID 本体"而不是"查找工具"。

**实际修法**（`projects/unityloader/javastubs/unity.cpp` 的 `getConstructorID`）：按**参数列表**
（忽略返回类型——请求是 `(...)V`，注册项是 `(...)L<class>;`）在 `clazz->methods` 里找到真正带 body 的
`<init>` 条目，把执行体拷进这个包装对象：

```cpp
ctor->nativehandle = body->nativehandle;
ctor->native  = body->native;
ctor->_static = true;
```

不改 `libjnivm`，因此**没有全局回归风险**（§6 那条 P0 回归风险对本修法不适用）。

→ 那条 446 行 `Unknown Static` 的 `Sig=` 打印的是**被调用对象自己的 signature**，不要误当作"查找键"来推理。

---

## 5. 已验证的确定性结论

1. **`RegisterNatives` 确实被大量调用**（修正 tag 后实测）：`org/fmod/FMODAudioDevice`、`com/unity3d/player/UnityPlayer`（20 个）、`ReflectionHelper`、`GoogleVrProxy`、`GoogleARCoreAPI`、`Camera2Wrapper`、`NativeLoader`、`bitter/jnibridge/JNIBridge`。
   **`android/util/DisplayMetrics` 上没有任何 `RegisterNatives`。**
2. **`DisplayMetrics` 的 `handle=0` 空桩**来源是 `GetMethodID` miss 分支的 `cur->methods.push_back()`，每条 miss 都会插一条（`find_if` 取第一个命中，所以空桩可能永久遮蔽后续注册项——这是真实隐患）。
3. `<init>` 查找与注册的签名在修复后**能正确匹配**，`DisplayMetrics`/`Handler` 等的 miss 归零。
4. 帧仍全黑**不是**因为 `getRealMetrics` 崩（已修）**也不是**因为 present 没发生（14 次 swap），而是**场景里没有内容可画**。
5. **`ReflectionHelper.getConstructorID` 返回的对象就是"jmethodID 本体"，不是查找工具**。Unity 把它直接喂给
   `NewObject*` 槽位，该槽位被 jnivm 分派为 **static** `CallMethod`，只有 `mid->dynamic` / `mid->nativehandle`
   两条成功路径。`javac.cpp` 的 `Constructor` 两者都不设，所以**任何**经由它构造的对象都会得到 null——
   与签名怎么写、`GetMethodID` 有没有做 `<init>` 改写无关。（已修，见 §4.4。）
6. `FakeJni` 注册的构造器一律是 **static** 条目，签名为 `"(args)L<class>;"`（返回类型是类自己，不是 `V`）。
   因此按**完整签名**去比对构造函数永远匹配不上；要比就比**参数列表**（截到 `)`）。这一点在写任何
   "手工解析 `<init>`"的代码时都要记住。


---

## 6. 当前阻塞点（按优先级）

### ✅ P0（已关闭 2026-09-21 晚）—— `AssetLocator.GetReaderWrapper(String)` 没有桩

> **当前状态：已用"惰性活对象"探针解掉阻塞**。实测 NRE 归零、8 个 AssetBundle **全部真实打开并打出大小**
> （`assets/Bundles/bundle1` 13 498 260 B … `bundle8` 4 256 915 B，全部 present），
> `Unable to read header from archive file:` 从 **8 条降到 0 条**。
> ⚠️ 但仍**只是探针**：读出来的内容还是默认值，接口签名靠日志逼出来的（`Seek(JI)J` / `Read(I)I` 已实测），
> 其余四个（`GetBytes`/`GetLength`/`GetPosition`/`Close`）**未见调用**、只有字面量池证据。
> 画面全黑**已不能再用"bundle 没读进来"解释**。以下为原始排查记录，保留备查。

**现象**：
```
[BD-JNIVM]: [STUB-MISS] Native MethodID: Class=`com/mobge/assetlocator/AssetLocator`
            Member=`GetReaderWrapper` Sig=`(Ljava/lang/String;)Ljava/lang/Object;` -> returning default
Unity: NullReferenceException
  at MobGe.Storage.NativeAndroidReaderWrapper.Seek (System.Int64 offset, System.IO.SeekOrigin origin)
  at UnityEngine.ManagedStreamHelpers.ManagedStreamSeek (...)
Unity: Unable to read header from archive file:        ×8
```

**根因链**：`MobGe.Storage.NativeAndroidReaderWrapper` 是 `System.IO.Stream` 的子类，内部持有
`AssetLocator.GetReaderWrapper(path)` 返回的 Java 对象（字段 `_javaObject`）。
该查找 miss → 返回 null → Unity 的 `ManagedStreamSeek` 调 `Seek()` → 解引用 null → NRE →
`AssetBundle.LoadFromStream` 读不到头 → 8 个 bundle 全废 → 启动停在这。

**已做的探针（保留在 `javastubs/bd_assetlocator.cpp`）**：`GetReaderWrapper` 暂时返回一个**惰性活对象**。
效果：**NRE 归零**，8 个 bundle 全部真实打开并打出大小
（`assets/Bundles/bundle1` 13,498,260 B … `bundle8` 4,256,915 B，全部 present）。
读出来仍是默认值，所以头还是解不出来 —— 但方法名+签名被日志逼出来了。

**需要实现的 Java API**（`[BD-ANY-MISS] cls=java/lang/Object` 实测 + 字面量池交叉验证）：

| 方法 | 签名 | 来源 |
|------|------|------|
| `Seek` | `(JI)J` | **日志实测**（`long Seek(long offset, int origin)`） |
| `Read` | `(I)I` | **日志实测**（`int Read(int count)`） |
| `GetBytes` | 未见调用 | 字面量池（`global-metadata` literal idx 5280） |
| `GetLength` | 未见调用 | 字面量池 idx 5283 |
| `GetPosition` | 未见调用 | 字面量池 idx 5284 |
| `Close` | 未见调用 | 字面量池 idx 5278 |

全局字面量里与 `IAssetReader` / `JavaReadResults` / `NativeAndroidReaderWrapper`
（string 表 idx 51190–51195）相邻的六个名字就是 `Close / Read / GetBytes / Seek / GetLength / GetPosition`。

**下一步**：把探针换成真实实现 —— 一个操作真实文件的 Java 侧 reader 类，
按上表注册（`GetBytes`/`GetLength`/`GetPosition`/`Close` 的签名先按最自然的猜，
第一次跑必然在 `[BD-ANY-MISS]` 里补全）。

### 🔴 P0-b（同场，新）—— 片头视频 URL 解不成真实文件

> ⚠️ 下面这段是**补齐 NDK 符号之前**的历史日志，保留作为症状对照。当前状态见紧随其后的"事实核对"。

```
Unity: StandaloneFullscreenVideois setupped.
Unity: AndroidMediaNDK could not load symbol AMediaFormat_toString, will stop loading NDK.
Unity: AndroidMediaNDK: libmediandk.so not loaded but expected to be present in API level 29 (>=21).
       NDK-based video playback disabled, will use JNI instead.
Unity: Cannot Prepare a disabled VideoPlayer
Unity: AndroidVideoMedia::OpenExtractor could not translate
       jar:file://!/assets/Videos/mobge_and_senri_splash_video.mp4 to local file.
Unity: AndroidVideoMedia: Error opening extractor: -10004
```

历史判断是"Unity 走 JNI 路径、JNI 路径要求 `jar:file://<apk>!/assets/...` 转本地文件"——
**这个判断已部分作废**：NDK 符号链已补齐（§0.5-A），`could not load symbol` 那条已不再出现。
但 `could not translate` / `-10004` 的症状本身仍有待在新配置下复核。
**确定的一点**：URL 里 host 部分为空（`jar:file://!`），说明 URL 拼装源头没拿到 obb/apk 路径。

**事实核对（2026-09-21 晚·第二场）**：

- 文件本身在 `gamedata/assets/Videos/mobge_and_senri_splash_video.mp4`（**979 746 B，确实存在**）。
- **NDK 符号链已补齐并生效**（§0.5-A），`could not load symbol` 已消失 → 不再是"退回 JNI 路径"的问题。
- rd8 日志里 `[BD-MEDIA] extractor new` 之后、`setDataSource` **之前**就是 Unity 自己打的翻译失败。
  也就是说**翻译这事由 Unity 原生代码做，发生在它调 `setDataSource` 之前**
  （`libunity.so` 内翻译函数 `0x4c124c`，错误串在偏移 `0xbde848`）。
- URL 里 host 为空（`jar:file://!`）的直接来源：**`getObbDir()`/`getObbDirs()` 返回 null**（§0.5-C）。

**待办方向（按优先级，已按本轮证据重排）**：

1. 🔴 **实现 `Context.getObbDir()/getObbDirs()`（+ `Activity.getObbDir(s)`）**，让 URL host 非空。
   这是本轮**唯一有直接日志支撑**的方向，且改动小、可在干净配置下独立验证。
2. 🔴 **重做"只改 `android_package_code`"的 A/B**（上一轮那个实验无效，见 §0.5-B），
   用来确认 host 到底取自 `sourceDir` 还是 obb 目录。可与第 1 条合成一轮。
3. ⚠️ **接线 il2cpp 探针**（§0.5-D 的四步），直接打印 `Application.dataPath` / `streamingAssetsPath` 真值。
   这是省时间的一步：打印为空 → 修 Java 侧；打印正确 → 说明 Unity 拼 URL 走的是别的输入。
4. 兜底（已做，但**至今没有机会生效**）：`thunks/ndk/media.cpp` 的 `bd_local_media_path()`
   会在 `AMediaExtractor_setDataSource` 入口把 `jar:file://<host>!/<asset>` 解成本地文件，
   候选顺序 `<host>/<asset>` → `<cwd>/<asset>` → `<cwd>/gamedata/<asset>`。
   ⚠️ 它**只在 host 非空时才有意义**；host 空时 `asset` 段仍在，理论上也能命中 `cwd/assets/...`，
   但因为 Unity 在调 `setDataSource` **之前**就放弃了，这段代码根本没被执行到。
5. 片头视频是**纯装饰**（`VikingMushroom/skip.png` 是跳过按钮）。若前三条都推不动，
   可考虑让它"立刻播完"——但**必须先确认**游戏不会因此少走一步初始化。

### ✅ P0（已关闭 2026-09-21）—— `AssetLocator.<init>` 解析失败

**根因**：不是签名、不是 `GetMethodID` 的改写，而是 `getConstructorID` 交出去的包装对象**没有执行体**。
修法见 §4.4，已实测通过。以下为原始排查记录，保留备查。

seq28 实测：

```
3368: getConstructorID(com/mobge/assetlocator/AssetLocator,
        (Lcom.unity3d.player.UnityPlayerActivity;Ljava/lang/String;)V) = 0x40030d343a90
3362: getConstructorID: ... methods=5          ← 注册表里 4 个构造器 + ListAssets，全部 handle=1
3366:   reg name='<init>'
        sig='(Lcom/unity3d/player/UnityPlayerActivity;Ljava/lang/String;)Lcom/mobge/assetlocator/AssetLocator;'
        static=1 native=(nil) handle=1        ← 正确的注册项就在表里
3370: [STUB-MISS] Unknown Static: ... AssetLocator Member=`<init>`
        Sig=`(Lcom.unity3d.player.UnityPlayerActivity;Ljava/lang/String;)V`   ← 注意：点号 + V，未改写形态
```

`ASSETLOC` 日志（`AssetLocator` 构造器里的 `BD_LOG`）**一条都没有** → C++ 构造器没跑 → `ListAssets` 也就无从返回条目 → `GetRelativeFilePaths` 拿到 null → `NullReferenceException`（本次已被后续流程掩盖，但根因未除）。

**关键线索**：被调用对象的 signature 是**点号 + `V`**，即**未经改写**的原始形态。而修复后的 `GetMethodID` 对任何 `<init>`+`V` 都会改写。所以这个 mid 不是 `GetMethodID` 创建的——需要新会话确认它是**哪个对象**：

- 怀疑方向 A：Unity 把 `getConstructorID` 返回的 `Constructor` 对象直接当 jmethodID 用（`Constructor::signature` 在 `javac.cpp` 里已归一化为**斜杠**+V，与实测的**点号**+V 不符，故此方向可疑）。
- 怀疑方向 B：另有路径用**未改写**的签名创建了 stub（例如某处 `MethodProxy` / `FindClass` 后的缓存），需要靠 `[BD-CTOR-ENTER]`（现在能看到全部 `<init>` 查找）与对象指针比对来定位。
- **下一步最有效动作**：在 `jinvoke` 的 static 分支（`Unknown Static` 那处）打印 `mid` 指针 + `mid->name/signature/_static/native/nativehandle`，并和 `getConstructorID` 里 dump 的注册项指针对照。两者一比对，来源立刻明确。

> ☝️ 这条"最有效动作"**已执行完毕**，结论是：两处指针完全一致（`mid == getConstructorID` 返回值），
> 且 `handle=(nil)`。所以"怀疑方向 A"是对的——原文因"实测是点号而 `Constructor::signature` 是斜杠"而排除了它，
> 那个矛盾来自 `[BD-CTOROBJ]`/`[UnityReflection]` 打印的是**入参**，不是对象自身的字段。

**修复后实测（`/game/Oddmar/log-seq.seqFIX` + `seqFIX/` 帧目录）**：

```
3378: getConstructorID: bound com/mobge/assetlocator/AssetLocator(Lcom/unity3d/player/UnityPlayerActivity;Ljava/lang/String;)V
                        -> com/mobge/assetlocator/AssetLocator(Lcom/unity3d/player/UnityPlayerActivity;Ljava/lang/String;)Lcom/mobge/assetlocator/AssetLocator;
3381: [BD-ASSETLOC] AssetLocator root arg 'com.mobge.Oddmar' -> assets
3382: [BD-ASSETLOC] AssetLocator(context=0x400008ceef80) root=assets
3391: [BD-ASSETLOC] ListAssets('SoundBanks') -> 153 entries from assets/SoundBanks
3396: [BD-ASSETLOC] ListAssets('SoundBanks/English(US)') -> 4 entries from assets/SoundBanks/English(US)
```

回归对照（同容器、同参数、同 32s；`log-seq.seqREV.bak` = 修复前，`log-seq.txt` = 修复后）：

| 指标 | before | after |
|------|--------|-------|
| `[STUB-MISS]` 条数 | 56 | 55 |
| STUB-MISS 集合差异 | — | **只少了 `AssetLocator.<init>` 一条，零新增** |
| 异常 / `terminate` / SEGV | 无 | 无 |
| 日志行数 | 26528 | 29536 |
| 末尾状态 | `Caught signal, fast-exiting via _exit` | 同 |

**仍未解决（不再是资源定位问题）**：

1. **帧仍全黑**：`frame.1..3.gl.ppm` 均 640×480、307200 像素**全为 `00 00 00`**（不是近黑，是纯黑）。
2. **32s 被音频吃掉**：`AssetLocator` 之后日志被 `stat(/game/Oddmar/conf/SoundBanks/*.wem)` 刷屏，
   是 Wwise 在解析 bank 路径；同时有 `AK_Fail` / "AkInitializer.cs Awake() was not executed yet"。
   说明启动很可能**还没走到场景内容挂载就超时了**——下一轮应先**加长运行时间**（如 `SECS=90`）
   或临时摘掉音频，把"黑屏"与"没跑够"分开。
3. **同类悬空构造器仍在**（同一病因、但那些类**根本没注册**，属另一条线 —— `GetMethodID` miss 造空桩，见 §5 第 2 条）：
   - `com/mobge/oddmarbilling/OddmarPurchaseHandler.<init>`（该 class `methods=0`，本仓库无此桩）
   - `android/media/AudioFocusRequest$Builder.<init>(I)`
   - `android/media/AudioAttributes$Builder.<init>()`
   - `java/lang/Object.<init>()Ljava/lang/Object;`（形态异常，值得单独看）

### P0 —— 回归风险：`libjnivm` 是全局改动

本次改了 `GetMethodID` 的核心行为（`<init>` 改写不再区分 `isStatic`、签名归一化）。**这会同时影响所有其他移植项目**。
**动手之前务必先跑一遍其他已能跑的游戏**（AGENTS.md / `docs/CASE_STUDIES.md` 里列出的那些），确认没退化。若有退化，优先考虑把归一化与改写收窄到"仅构造器 + 仅该端口"的范围。

### P1 —— 残留 `STUB-MISS`（去重后各 1 条）

> **按上面的目标约束，本节大部分不需要修。** 只处理「会升级成致命异常」的；
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
- `java/lang/StackTraceElement.<init>(...)`、`java/lang/Error.<init>(String)`

> 注：`getAssets`/`getPackageManager`/`getIntent` 这类"在基类上"的，理论上已被 Method 1b 覆盖，仍 miss 说明 **1b 未覆盖到该调用点**，值得复查。

#### ⚠️ 已核实：`getApplicationContext` 的修复**没有生效**，别照着它抄

`javastubs/android.h` + `android_content.cpp` 新增了 `Context::getApplicationContext()` 并注册，
注释里的诊断是对的（Unity 用**泛型**签名查），但修法解决不了问题：

```
查找键（Unity） : str0='getApplicationContext' str1='()Ljava/lang/Object;'   static=0 与 static=1 都试过
注册项（我们） : name='getApplicationContext' sig='()Landroid/content/Context;' static=0 handle=1
```

**两侧签名不匹配 → 照旧 miss。** 实测 miss 次数 **before = 72 / after = 72，一条没少**
（`log-seq.seqREV.bak` vs `log-seq.txt` 都是 72）。新增的 `getApplicationContext -> 0x…` 日志出现 3 次，
说明 C++ 实现偶尔被匹配上，但**没有消除 miss**。

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

## 7. 建议的下一步（按顺序）

### 7.0 ⭐ 本轮（2026-09-21 晚·第二场）之后的顺序 —— 从这里开始

> 前置：先做 **§⛔ 接手第 0 步**（还原 `unity.toml`、确认 `BOOT_LOADER`）。
> 以下第 1 组三件事**可以并成一轮**做，因为它们都为了同一件事：搞清楚 `jar:file://` 的 host 从哪来。

**第 1 组（本轮最高优先级，一轮出结论）**

1. 🔴 **实现 `Context.getObbDir()` / `getObbDirs()`**，并补 `Activity.getObbDir(s)` / `getObbDirs()`。
   返回 `File(android_obb_dirs[0])`（`= "./"`），让 Unity 拼出的 URL 有 host。
   - 参考实现位置：`javastubs/android_content.cpp`（`Context` 的其它方法就在那），
     声明加在 `javastubs/android.h`。
   - `java/io/File` 已有实现，直接构造即可；数组返回参考 `splitPublicSourceDirs` 的写法
     （`android.h:840`）。
   - **验收**：日志里 `getObbDirs` 的 `[STUB-MISS]` 消失，且视频 URL 变成 `jar:file://<host>!/assets/...`（host 非空）。
2. 🔴 **重做 A/B，但只改 `android_package_code`**（上一次混入了 `android_source_dirs=[]`，实验无效，见 §0.5-B）。
   建议三次对照，每次都保证 `android_source_dirs=["./"]`：

   | 轮次 | `android_package_code` | 看什么 |
   |------|----------------------|--------|
   | A（基线） | `"./"` | URL host 是否为空 |
   | B | `"/tmp/ZZZSENTINELPACK.apk"` | host 是否跟着变成那个哨兵串 → 判定 host 是否来自 package code |
   | C | `"/game/Oddmar/gamedata"`（真实目录） | host 变成真实目录后视频能否播 |

   ⚠️ 判读时**必须先确认该轮跑到 `init time` + 8 bundle**，否则和上次一样白跑。
3. ⚠️ **接线 il2cpp 探针**（§0.5-D 的四步改法），打印 `Application.dataPath` / `streamingAssetsPath` 真值。
   这是第 1、2 条的"裁判"：如果 Unity 自己报的 `streamingAssetsPath` 就是 `jar:file://!/assets`，
   那问题在 Java 侧的路径供给；如果报的是正确路径，说明 URL 由别的输入拼装，转向查 `libunity.so` 的
   `GetDataPath` 链（`0x39f800 → 0x4bfbe8 → 0x4bbe14`）。

**第 2 组（第 1 组跑通后再动）**

4. 画面出内容后，再回来看 `frame.*.gl.ppm` 是否出现非零像素；若仍黑，转查相机/场景/AssetBundle 挂载。
5. 云服务回调（§0.5-F）：确认 `SocialImpl.authenticate` 是否会卡住 `MGLOProgressData.construct` 链。
   断网约束下只需"不阻塞"，**不要**实现功能语义。
6. `[BD-ANY-MISS]` 噪声降级、§4.2 诊断日志清理、`GetMethodID` miss 时 `push_back` 空桩遮蔽后续注册
   的结构性隐患（这条是 `libjnivm` 全局改动，做之前先补回归）。

**第 3 组（本批改动收尾）**

7. **`libjnivm` / 全局改动的回归**：本批累计改了 `method.cpp`、`vm.cpp`、`findclass.cpp`、
   `javastubs/android*.cpp`、`thunks/ndk/*`。§7.1 第 1 条的 4 组对照是**更早一批**的结论，
   本批新增改动（尤其 `thunks/ndk/*` 的 `AMediaFormat` 结构体加了 `buffers`/`text` 字段、
   `getPackageCodePath` 改走 `bd_compute_source_dir()`）**还没回归过**，建议用 Samurai2/Maximus2 补一对。
8. 收尾清理：容器内 50 个 `seq*` 目录 + 7 个 `log*.bak`（§9）；宿主机那份半成品探针要么接线要么撤掉。

### 7.1 此前列出的（部分已完成）

1. ✅ **回归已完成（2026-09-21）**，用两个**已发布端口**做真对照：
   `Samurai2` / `Maximus2`（`D:\Locke\gitee\LinuxArmPorts\武士2复仇.zip` / `街头角斗士2.zip`，
   包内自带 loader = **改动前基线**；另编 `build-regress` = **改动后**；同容器、同 Xvfb、`SECS=60`）。
   **4 组全部跑满 60 s、零 `[BD-SEGV]` / `terminate`**；两个游戏都到 GL 初始化，
   `nativeRender ×22`、`eglSwapBuffers ×24–29`；Samurai2 的插件 `samurai2_offline.so` 正常 `init`
   （**plugin ABI 未变**），Maximus2 `PlayerPrefs` 存下 127 条。
   → **这批改动没有破坏既有端口**，`method.cpp` 的全局改动可保留。
   复现：容器内 `/regress-run.sh`、`/regress-all.sh`（用法见脚本头注释）。
2. ~~**定位 `AssetLocator` 那个 mid 的来源**~~ → **已完成**：`mid == getConstructorID` 返回值，`handle=(nil)`；
   根因是包装对象没绑执行体，已修（§4.4）。
3. ~**加长运行时间，先把"黑屏"和"没跑够"分开**~ → **已完成**：`SECS=90` / `SECS=120` 都试过，
   进程稳定跑满、210–227 帧，**画面依然 192 B 纯色** → 结论是"不是没跑够"，是真的没有内容可画。
   音频刷屏也不再是障碍（`rd5–rd8` 都跑满）。摘掉 Wwise 复跑的对照**仍未做**，但优先级已降。
4. **顺带修一个结构性隐患**：`GetMethodID` miss 时无条件 `push_back` 空桩，会让第一个 miss 永久遮蔽同名同签名的**后续**注册（`find_if` 只取第一个）。建议改成"复用同名同签名条目并填充"而不是新增。
   —— 注意这是 `libjnivm` 全局改动，做之前请先补第 1 条的真实回归。
5. **`[BD-ANY-MISS]` 噪声要降级**：它在 `GetMethodID` miss 分支**无条件**打印（源码里 `[STUB-MISS]` 才是按 `trace` 打的），
   而 `Class::getMethod` 会用"member + static"双向探测，所以**一次成功的** `getMethod` 也会吐 1~2 行 miss
   （`Choreographer$FrameCallback.doFrame`、`JNIBridge.invoke` 每帧都刷）。本次日志从 26528 涨到 29536 行，
   增量几乎全是它。建议加 `if(trace)` 或改成计数摘要。
6. ~处理 P1 里与主循环/资源相关的 `Activity.getObbDir(s)`~ → **已升级为 §7.0 第 1 条（本轮最高优先级）**。
   其余的 `Intent.getExtras`、`Bundle.getBoolean`、`Window.getAttributes`、`SurfaceView.*` 仍留在 P1。
7. 画面真正出内容后，再看 `frame.*.gl.ppm` 是否出现非零像素；若仍黑，再查相机/场景/AssetBundle 挂载。
8. 清理 4.2 的诊断日志（`getConstructorID` 的注册表 dump 现在多了一行 `bound ...`，一并清）。


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
| `BD_PROBE_APPPATHS=1`（env） | ⚠️ **尚未接线**，见 §0.5-D。接线后打印 `Application.dataPath` 等真值的 `[PATH-PROBE]` |

**自检命令**（确认二进制里真的有你以为的代码 —— 这一条本轮又救了一次）：

```bash
# 本轮用的 loader 是 build-regress（RelWithDebInfo，14:31）
grep -c "你的日志字符串" /workspace/Bogodroid/build-regress/unityloader

# 反例：探针只写在宿主机源码里、没同步进容器、也没重编 → 这里会是 0
grep -c "PATH-PROBE" /workspace/Bogodroid/build-regress/unityloader     # 实测 = 0
```

**构建目录速查（2026-09-21 晚实测）**：

| 目录 | 类型 | 关键开关 | `unityloader` 时间/体积 |
|------|------|---------|----------------------|
| `build-regress` | **本轮在用** | RelWithDebInfo / LOG=ON / VERBOSE=OFF / NON_ZERO=OFF | 09-21 14:31，152 MB |
| `build-oddmar` | **旧产物，勿用** | Release / LOG=ON | 09-21 07:55，6.3 MB |
| `build-oddmar-sym` | 备用 | RelWithDebInfo | 09-21 08:04，144 MB |
| `build-oddmar-verbose` | 备用（上一轮在用） | RelWithDebInfo / VERBOSE=ON | 09-21 11:48，150 MB |
| `build-anbernic-debug` | 上机调试 | Debug / LOG+TRACE=ON | 09-16 08:33，102 MB |
| `build-anbernic-rel` | **发版用** | Release / LOG=OFF | 09-18 07:22，6.2 MB |

> 七个目录的 `JNIVM_ENABLE_RETURN_NON_ZERO` 实测**全部为 `OFF`**（已逐个 grep 过 `CMakeCache.txt`）。
> 即便如此，**每次构建仍要显式带 `-DJNIVM_ENABLE_RETURN_NON_ZERO=OFF`**（AGENTS.md 的 CMake 缓存陷阱）。

---

## 9. 容器内可复用资产与待清理项（2026-09-21 晚实测）

### 9.1 脚本（都在容器 `/` 下，直接 `bash /xxx.sh` 调用）

| 脚本 | 用途 |
|------|------|
| `/run-oddmar-seq.sh` | **主用**。逐帧截 X 根窗口 + loader `glReadPixels` dump。可用 env 覆盖：`SECS`（默认 32）、`SEQ_DIR`、`BOOT_LOADER`、`BD_DUMP_FRAME_AT` |
| `/run-any-seq.sh` | 通用版（换游戏用），同上参数 |
| `/run-oddmar.sh` | 精简版（无逐帧截图） |
| `/regress-run.sh` / `/regress-all.sh` | 回归对照（Samurai2 / Maximus2），用法见脚本头注释 |
| `/probe-oddmar-window.sh` | X 窗口探测 |

### 9.2 本轮标准运行命令（照抄即可）

```bash
docker exec GlES_Dev bash -lc '
  cp /game/Oddmar/unity.toml.bak-rd9 /game/Oddmar/unity.toml     # 先还原（§⛔ 0-1）
  cd /game/Oddmar &&
  GAME_ROOT=/game/Oddmar SEQ_DIR=/game/Oddmar/seq-rd10 SECS=120 \
  BOOT_LOADER=/workspace/Bogodroid/build-regress/unityloader \
  bash /run-oddmar-seq.sh > /game/Oddmar/rd10.out 2>&1; echo "exit=$?"'
```

判读三件套（**必须同时看**，否则会像上一轮那样误判）：

```bash
L=/game/Oddmar/log-seq.txt
grep -c "init time" $L                    # 必须 >0，否则这轮没跑到关键点，结论无效
grep -n "BD-ASSETLOC" $L | head           # 必须出现，否则 AssetLocator 没构造
grep -n "getObbDirs\|could not translate\|-10004" $L | head   # 本轮的观察目标
awk '{print $4}' /game/Oddmar/seq-rd10/timeline.txt | sort -u | head   # 192 B 之外才有画面
```

### 9.3 待清理（新会话建议先清，避免误读旧产物）

- **`/game/Oddmar/seq*` 共 50 个目录**：`seq`、`seq2`…`seq90`、
  `seqBASE/BASE2/C32/DEEP/FIX/INERT/LATE/PHANTOM/PROXY/RD2/RD3/RD4/READER/REV/SPLASH`、
  `seq-rd5`…`seq-rd9b`。全部是历次运行的截图与 timeline，**没有源头价值**。
- **`/game/Oddmar/log*.bak` 7 个**（含 11.9 MB 的 `log-seq.seq90.bak`）：
  `log-seq.before90.bak`(12:17)、`log-seq.seq28.bak`(11:33)、`log-seq.seq90.bak`(12:20)、
  `log-seq.seqC32.bak`(12:25)、`log-seq.seqREV.bak`(11:47)、`log.txt`(08:51)、`log-probe.txt`(09:18)。
  其中 **`seqREV.bak` / `seq28.bak` 是 `getConstructorID` 修复的前后对照，`before90.bak` 是回归基线** ——
  确认不再需要再删。
- ⚠️ **`/game/Oddmar/unity.toml.bak-rd9` 不要删**：它是唯一一份正常配置备份。
- 宿主机 `/tmp/ZZZSENTINELPACK.apk`（0 字节）留在容器里无所谓，但**别把它当真实 APK 推理**。

清理建议（**先只列、确认后再删**）：

```bash
# 1) 先看要删什么
docker exec GlES_Dev bash -lc 'ls -d /game/Oddmar/seq* | wc -l; ls -la /game/Oddmar/log*.bak'

# 2) 确认无用了再删（seq* 全是历次截图/timeline，可整体清）
docker exec GlES_Dev bash -lc 'rm -rf /game/Oddmar/seq /game/Oddmar/seq[0-9]* /game/Oddmar/seq-rd[0-9]* \
  /game/Oddmar/seqBASE /game/Oddmar/seqBASE2 /game/Oddmar/seqC32 /game/Oddmar/seqDEEP \
  /game/Oddmar/seqFIX /game/Oddmar/seqINERT /game/Oddmar/seqLATE /game/Oddmar/seqPHANTOM \
  /game/Oddmar/seqPROXY /game/Oddmar/seqRD[0-9] /game/Oddmar/seqREADER /game/Oddmar/seqREV \
  /game/Oddmar/seqSPLASH'

# 3) 日志备份：只删确定不再需要的，保留 unity.toml.bak-rd9
#    seqREV.bak / seq28.bak = getConstructorID 修复前后对照；before90.bak = 回归基线
```

---

## 10. 相关文档

- `AGENTS.md` —— 日志/构建开关、Dropbeak 推拉文件
- `docs/PORTING_PLAYBOOK.md` —— 端口化通用流程、§1.1 构建命令、§1.2 `JNIVM_ENABLE_RETURN_NON_ZERO`、§1.3 缓存项
- `docs/CASE_STUDIES.md` —— 案例四"悬垂的 jmethodID"（本次核心根因之一）
- `.workbuddy/memory/2026-09-21.md` —— 当日工作日志
