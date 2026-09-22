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

## 0.6 第三场（2026-09-22 白天）：分支 `ddmar` + 两个绕过点，以及 hook 契约的定论

> 本场**没有解决黑屏**，但把"为什么这个 hook 一定不生效"钉死了：**打的是一个虚调用转发 thunk，
> 它的契约是 `bool` + 两个输出槽，而现在返回的是指针。** 同时更正了上一场三条既有结论。

### A. 分支与提交

- 新分支 **`ddmar`**（从 `video` 切出）。切出时工作区干净，两个提交都在这条分支上。
  （**本文件这一版的改动本身尚未提交**，见 §13 版本行。）
- `beab38d` *oddmar: continue offline video startup fixes* — 34 个文件：两份文档、`javastubs/android*.{h,cpp}`、
  `bd_assetlocator.cpp`、`javac.cpp`、`libjnivm` 三处（`method.cpp`/`vm.cpp`/`findclass.cpp`）、
  `thunks/ndk/media*`+`ndk.cpp`、`libc/fcntl.cpp`+`misc.cpp`+`stdio.cpp`、`platform/common/debug_utils.*`、
  `projects/unityloader/main.cpp`+`javastubs/*`、`scripts/container-run-game-seq.sh`。
- `eb68cf7` *oddmar: add opt-in Unity video translation bypass* — `projects/unityloader/main.cpp`(+37)、
  `thunks/libc/fcntl.cpp`(+27/−3)。

### B. 本场新增的两个"离线视频绕过点"

**B-1. `clean_jar_path()` 增加 `<cwd>/gamedata/assets` 映射**（`thunks/libc/fcntl.cpp:82-101`）

通用清理把 hostless 的 jar 前缀 `jar:file://!` 映射成 `<cwd>/assets/...`；新增块在这之上找 `/assets/`，
若 `<cwd>/gamedata/assets/...` **`stat` 存在**就改返回它。

⚠️ **实测这个映射本轮一次都没被行使**：本场日志里对视频文件**零 `stat`/`open`**（`grep "Videos/mobge_and_senri_splash_video" log-args3.txt` 只有 2 条：hook 的打印 + Unity 的失败行）。
原因见 D：Unity 的翻译步骤发生在**文件层之前**就返回了"失败"。所以它是"备用"，只有在 Unity 真的去开文件时才有意义。

**B-2. `BD_BYPASS_VIDEO_TRANSLATE=1`：hook Oddmar `libunity.so + 0x4c124c`**

- 实现：`projects/unityloader/main.cpp:34-60`（`bypass_video_translate`）+ `main()` 里 `~1166` 处
  `hook_address_detour(&lunity, addr_lunity + 0x4c124c, …)`，**只在环境变量存在时装**，默认路径不受影响。
- 实测触发 **1 次**（与视频翻译被调用次数一致），路径也打印对了（`/game/Oddmar/gamedata/assets/Videos/mobge_and_senri_splash_video.mp4`，文件确实存在 979 746 B），
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

⇒ **实质签名**：`bool f(wrapper* ctx, const <path>* in, <out ptr>, <out len>)`；
返回 **bool（`w0` 的 bit0）**，结果走 **a2/a3 两个相邻 8 字节输出槽**。
**不是 sret**：调用前 x8 未被设置，而 thunk 自己把 x8 当暂存（`mov x8, x3`）→ 不可能是"结构体按值返回"。

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

### D. 为什么现在的 hook 必然失败 —— 两条都是硬性的

1. **返回类型错位**：现在 `return reinterpret_cast<uintptr_t>(g_bypass_video_path.c_str())`，
   而调用方用的是 **`tbz w0, #0`**（测 **bit 0**，不是测非零）。`std::string::c_str()` 至少 8 字节对齐
   ⇒ **低位恒为 0 ⇒ 恒判"翻译失败"**。用"返回指针"去满足"返回 bool"的接口，永远为假。
2. **输出槽没写**：即使把 bit0 凑成 1 也没用 —— a2/a3 指向的两个槽**从未被写**（ptr 槽还被 `str xzr` 预清零），
   调用方随后 `ldp` 读到 `{null, 陈旧值}`，还要拿那个 size 做边界检查 → 只会更糟。

这两条合起来，正好逐字解释日志现象：**`bypassing … -> <正确路径>` 打了，`could not translate` 照旧**。

### E. 正确修法（下一步，尚未实施）

```cpp
// 真实 ABI：a0 = 包装对象, a1 = 输入路径, a2 = 输出 ptr 槽, a3 = 输出 len 槽；返回 bool
static uintptr_t bypass_video_translate(uintptr_t a0, uintptr_t a1,
                                        uintptr_t a2, uintptr_t a3)
{
    if (a2) *(const char**)a2 = g_bypass_video_path.c_str();
    if (a3) *(size_t*)a3     = g_bypass_video_path.size();
    return 1;                       // ← bool true，不是指针
}
```

- 返回 **`1`**；路径字符串必须**常驻生命周期**（`static std::string` 的 `c_str()` 满足；别用临时量）；
- 别碰 a0/a1（a0 是栈上的包装对象）；a4–a7 是残值，别当参数。

### F. 本节更正的三条既有结论

1. **§0.5-C 的因果链被证伪。** `Context.getObbDir()/getObbDirs()` **已经实现**（在 `beab38d` 里，
   （`javastubs/android_content.cpp:797-826`，`Activity` 侧 `android_misc.cpp:175-176`，
   由 `bd_compute_obb_dir()` 读 `[paths] android_obb_dirs`），实测
   `[BD-DATADIR] getObbDir -> /game/Oddmar/gamedata`（`log-args3.txt:668`、`7373`）
   ——**但视频 URL 依旧是 hostless 的 `jar:file://!/assets/...`**。
   ⇒ "obb 返回 null ⇒ host 为空" **不成立**，URL 的 host 另有来源。
   （附带：`javastubs/android_content.cpp:934` 那条注释 "getObbDir / getObbDirs -> STUB-MISS path returns null
   (same as before)" 已经过期，实现就在它上面 100 行。）
2. **§0.5-F 的 `[JNIVM] Invalid Reference, Unexpected Type` 与 `JNIVM_ENABLE_RETURN_NON_ZERO=ON` 无关。**
   本场两次复现证明它由**哨兵 `unity.toml`** 触发（证据见 §⛔ 0-1）。
3. **"已加入的参数日志"在上一场一次都没打出来。** 全仓历史日志 `grep -c "video translate args"` = **0**
   （`log-hook-test{,2,3,4}.txt` 全是 `bypass=1 / args=0`）——那两行是在**最后一次运行之后**（容器内 05:12）
   才进源码并重编的。本场已补跑拿到真值（见 C）。

### G. 本场实测表

| 轮次 | 配置 | 结果 |
|---|---|---|
| hook-test 04:42 | 正常 toml | `bypassing`（路径为空）；无 args 行 |
| hook-test2/3 04:47 / 04:50 | 正常 toml | 路径拼成 `<cwd>/gamedata/gamedata/…`（cwd 已含 `gamedata`，多拼一级） |
| hook-test4 04:54 | 正常 toml | 路径正确；仍 `could not translate`；40 036 行、`init time`=1、8 bundle 正常 |
| **args1 / args2**（本场） | **哨兵 toml** | **两次完全一致：1 357 / 1 358 行、`init time`=0、`ZZZSENTINEL`=12、`Invalid Reference`×2** |
| **args3**（本场） | 正常 toml + `BD_BYPASS_VIDEO_TRANSLATE=1` | 79 168 行、`init time`=1、`Unable to read header`=0、hook 触发 1 次、**拿到 args 真值**；帧仍全 192 B |

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

---

## ⛔ 接手第 0 步（不看这四条会白跑一轮）

> 以下四条都是**当前环境的真实状态**，不是建议。全部核对于 2026-09-21 晚·第二场结束；
> **0-1 已于 2026-09-22 第三场复核并补上硬证据**（见 §0.6-G），其余三条未变。

**0-1. 容器里的 `unity.toml` 会被留成"哨兵"状态 —— 开工第一件事是核对它**

**2026-09-22 新增硬证据**：哨兵配置必然让启动死在 **1 35x 行**左右，两次跑完全一致（§0.6-G 的 args1/args2）。
先核对，再决定要不要还原：

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

> 2026-09-22 第三场已照此还原；**哨兵版另存为 `/game/Oddmar/unity.toml.sentinel-0922`**（没删，做对照用）。
> 注意 `unity.toml` 与 `unity.toml.bak-rd9` 的 mtime 都是 09-21 14:42 —— **别靠 mtime 判断当前是哪个版本，只能 grep 内容**。

**哨兵态的死法（本场两次复现，别再误判成 `JNIVM_ENABLE_RETURN_NON_ZERO=ON`）**：

```
[NATIVE] stat(/tmp/ZZZSENTINELPACK.apk/assets/bin/Data/globalgamemanagers)
[JBRIDGE] RunOnUiThread Running runnable!          ← Unity 在弹错误对话框
[JNIBridge] Invoking native handle … for java/lang/Runnable->run
Expected N5jnivm7android3app31DialogInterfaceOnCancelListenerE
[JNIVM]: Exception with Message `Invalid Reference, Unexpected Type` was thrown
[BD-PREFS] flush_all: wrote 1 dirty file(s)        ← 日志到此为止，之后由 timeout -s INT 收尾
```

典型计数：**1 35x 行**、`init time` = **0**、`Unable to read header` = 0、`BD-ASSETLOC` = 0。
（与 §0.6-F 第 2 条同源：`Invalid Reference` 是哨兵的连带症状，不是缓存项污染。）

**0-2. `BOOT_LOADER` 默认指向旧产物**，每次运行都必须显式传（见 §1.4 第 1 条）。

**2026-09-22 更新**：要跑"视频 hook / `PATH-PROBE` 探针"就必须用
`/workspace/Bogodroid/build-oddmar-verbose/unityloader`（容器 09-22 05:12，**含 hook + 参数日志 + 探针字符串**）；
`build-regress`（09-21 14:31）**三样都没有**，跑它拿不到任何相关日志。

**0-3. `projects/unityloader/main.cpp` 里的 il2cpp 探针：已在源码与二进制里，但*依然没接线***

**2026-09-22 更新**：宿主 md5 与容器 md5 **现在一致**（都是 `e16f8c132c7ea8b4eac4730efa05a66b`，随 `beab38d` 同步过去），
且 `grep -c "PATH-PROBE" build-oddmar-verbose/unityloader` = **4**（代码在二进制里）。
但**四处断点一个都没补**（行号为本版实测）：

1. `main.cpp:209` `static bool g_probe_app_paths = false;` —— **无人从 `BD_PROBE_APPPATHS` 赋值**；
2. `init()` 里**没有**解析 `il2cpp_runtime_invoke`（`grep -n "il2cpp_runtime_invoke\s*=" main.cpp` 无命中）；
3. `main.cpp:440` 早退条件仍是不含探针的 `if (g_n == 0 && g_post_init_n == 0) return;`；
4. `probe_app_paths(` 在 `main.cpp:344` 只有定义、**没有任何调用点**。

⇒ 想用探针，仍要按 §0.5-D 的四步改（改之前先读 §7.0 第 1 组，顺序有变）。

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
（2026-09-22 补充：`android_source_dirs=[]` 必然致死已由两次复现坐实，见 §0.6-G 的 args1/args2；
这条 A/B 已排进 §7.0 第 0-b 步。）

### C. 🔴 更正后的**真嫌疑**：`getObbDir()` / `getObbDirs()` 是 STUB-MISS 返回 null

> ⛔ **本节结论已于 2026-09-22 被证伪，保留作为排查记录。** `getObbDir`/`getObbDirs` 已实现并返回
> `/game/Oddmar/gamedata`，**URL 依旧 hostless** ⇒ "obb 为 null ⇒ host 为空"不成立。详见 §0.6-F 第 1 条。

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

> ⛔ **2026-09-22 更新：这条已经做了，但没解决问题。** 实现见 `javastubs/android_content.cpp:797-826`
> （`bd_compute_obb_dir()` 读 `[paths] android_obb_dirs`）+ `android_misc.cpp:175-176`（`Activity` 侧），
> 实测 `[BD-DATADIR] getObbDir -> /game/Oddmar/gamedata`（`log-args3.txt:668`、`7373`），
> **而视频 URL 仍是 `jar:file://!/assets/...`**。
> ⇒ 别再沿着"让 obb 非空"往下推；host 的来源还没找到，但**已经可以排除 obb 这一支**。
> （另：上面列的 3 条注释里，`android_content.cpp:789` / `:896` 和 `android.h:979` 那两句**已经随 09-22 的改动消失**，
> 全仓现在只剩 `android_content.cpp:934` 一句写着 "STUB-MISS path returns null"，而它就压在实现上面 100 行。）

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

> ⚠️ **2026-09-22 更新**：上面这四处的**行号已变**（本版实测：`g_probe_app_paths` 在 `main.cpp:209`、
> `probe_app_paths()` 定义在 `:344`、早退条件在 `:440`），**但断点一个都没补**。
> 探针源码现在**已经同步进容器**、并编进了 `build-oddmar-verbose`（`grep -c "PATH-PROBE"` = 4），
> 只是永不 arm。详见 §⛔ 0-3。

**要启用探针需要改的四处**（都在这一个文件里，按本版行号）：

- `main.cpp:209` 改成 `g_probe_app_paths = std::getenv("BD_PROBE_APPPATHS") != nullptr;`（或放在 `init()` 入口）
- `init()` 里补 `il2cpp_runtime_invoke = (p_runtime_invoke)so_symbol(lil2cpp, "il2cpp_runtime_invoke");`
- `main.cpp:440` 早退条件改成 `if (g_n == 0 && g_post_init_n == 0 && !g_probe_app_paths) return;`
- `il2cpp_init_hook()` 里 `install_once();` 之后加 `probe_app_paths("post-init");`

> 探针价值：如果 **`dataPath` 打印出来就是空/非法**，说明根因在 Java 侧（`sourceDir`/`obb`）；
> 如果 **打印出来是正确路径**，那就坐实"Unity 拼 URL 用的不是这两个属性"，直接转去查别处，省一轮。
> （2026-09-22 补充：`obb` 这一支已被排除，探针现在是**最直接的裁判**，见 §7.0 第 1 组。）

### E. 本轮实测数字

| 轮次 | 配置 | exit | X 截图帧数 | 截图大小 | `init time` | 8 bundle |
|---|---|---|---|---|---|---|
| rd5–rd8 | 正常 | 124（跑满） | 210–213 | **全 192 B** | 有 | ✅ 已加载 |
| rd9 | 哨兵 45s | 124 | 88 | 全 192 B | **0** | **0** |
| rd9b | 哨兵 120s | 124 | 227 | 全 192 B | **0** | **0** |

- 全部无 `[BD-SEGV]`、无 `terminate`。
- **192 B = 纯色**（`import -window root` 截的 X 根窗口，§10 案例 A 里 180 KB+ 才算有画面）。
- 哨兵两轮的 `seq-rd9*/` 里 **`frame.*.gl.ppm` 一个都没有**（`eglSwapBuffers` 未触发 dump）。

### F. 同期发现、但**不属于视频线**的阻塞

| 现象 | 事实核对 | 处理建议 |
|------|---------|---------|
| 云服务回调不触发 | `SocialImpl.authenticate` / `SaveGames.isConnected` 是 `[STUB-MISS]`、`entries=0`（类未注册）→ 回调查不到。**不是轮询卡死**（各仅 1 次 / 12 次调用） | 断网约束下只需"不阻塞"。若证实它卡住 `MGLOProgressData.construct` 链再修 |
| Wwise 起不来 | `WwiseUnity: Failed to initialize the sound engine. Reason: AK_Fail`，且 `AkInitializer.cs Awake() was not executed yet` 每帧刷 | 不崩，优先级最低 |
| `[JNIVM] Invalid Reference, Unexpected Type` | rd9b 出现 2 次（line 507/509）。**症状与 `JNIVM_ENABLE_RETURN_NON_ZERO=ON` 的经典崩溃一模一样**，但 `build-regress/CMakeCache.txt` 里明确是 `OFF` | ✅ **2026-09-22 已复核并结案**：只在**哨兵配置**下出现，是连带症状，与缓存项无关（§0.6-F 第 2 条） |

---

## 0. 当前状态速览

> **最后一次更新：2026-09-22 白天·第三场**（分支 `ddmar`）。目标约束见文首：**只需断网单机可跑**，
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
| **主阻塞（2026-09-22 重定位）** | **Unity 侧"视频路径翻译"调用的返回契约没被满足**：`libunity.so+0x4c124c` 是个虚调用转发 thunk，真实签名是 `bool f(ctx, in_path, out_ptr, out_len)`；现在 hook 返回的是 `char*`（对齐指针 bit0 恒 0）→ 调用方 `tbz w0,#0` 判"失败" → `could not translate` → `-10004`（§0.6-C/D） |
| **旧结论（已证伪）** | ~~`Context.getObbDir()` / `getObbDirs()` STUB-MISS 返回 null → URL host 为空~~：这两个方法 **09-22 已实现**并实测返回 `/game/Oddmar/gamedata`，**URL 依旧 hostless**，因果不成立（§0.6-F 第 1 条） |
| **次阻塞** | 云服务 `SocialImpl.authenticate` / `SaveGames.isConnected` 类未注册 → 回调不触发（§0.5-F） |

**一句话**：黑屏已推到最后一层——**引擎本身健康**（跑满 120 s、8 个 bundle 都读进来了、NDK 视频通路已启用），
唯一挡住画面的仍是片头视频那条链：URL 的 host 为空（`jar:file://!/assets/...`），
而 09-22 新加的那个 Unity hook **打在了转发 thunk 上、返回类型也对不上**，所以"看着触发了、其实恒判失败"（§0.6-C/D）。
**下一步最省事的动作是把 hook 改成写 a2/a3 两个输出槽并 `return 1`**（§0.6-E），
它比重查 host 来源更便宜、且能立刻给出"成败"的干净判据。
另外两条更正：① `getObbDir` 已实现、旧因果链作废；② `[JNIVM] Invalid Reference, Unexpected Type`
由**哨兵 `unity.toml`** 触发，与 `JNIVM_ENABLE_RETURN_NON_ZERO` 无关（§0.6-F）。

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
| `libjnivm/src/jnivm/vm.cpp` | `RegisterNatives` 增进程级 keepalive 表（`bd_registered_method_keepalive()`），pin 住注册的 `Method` | 修"悬垂 jmethodID"：`UnregisterNatives` 释放后，同尺寸 `Method` 复用同一块内存，缓存的 id 会安静地变成一个**合法但错误**的函数。见 §11 案例 B |
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
**动手之前务必先跑一遍其他已能跑的游戏**（`AGENTS.md` / `docs/PORTING_PLAYBOOK.md` §6 复测清单里列出的那些），确认没退化。若有退化，优先考虑把归一化与改写收窄到"仅构造器 + 仅该端口"的范围。

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
  （⚠️ 2026-09-22：`getObbDir` / `getObbDirs` 已在 `Context` 与 `Activity` 两侧实现并注册，
  `[BD-DATADIR] getObbDir -> /game/Oddmar/gamedata` 实测有值；若日志里还见它们的 miss，说明是**旧的调用点缓存了空桩**，见 §5 第 2 条）
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

### 7.0 ⭐ 第三场（2026-09-22）之后的顺序 —— 从这里开始

> 前置：**先做 §⛔ 接手第 0 步**（核对/还原 `unity.toml`、确认 `BOOT_LOADER`、确认二进制里真有你要的字符串）。
> **动手前先读 §0.6**：那里已经把那个 Unity hook 的调用契约钉死了，别再重复踩。
> 判读任何一轮，都必须同时看 `init time` / `Unable to read header` / `ZZZSENTINEL` 三个计数（见 §9.2）。

**第 0 组（半天内能出结论，最省事，优先）**

0-a. 🔴 **把 `BD_BYPASS_VIDEO_TRANSLATE` 的 hook 改成真实 ABI**（改法见 §0.6-E，
   代码在 `projects/unityloader/main.cpp:34-60`）：写 a2（输出 ptr 槽）/ a3（输出 len 槽），**`return 1`**。
   - **验收（三条同时看）**：`[BD-MEDIA] bypassing …` 之后**不再出现** `could not translate`；
     日志里出现对 `gamedata/assets/Videos/….mp4` 的 `stat`/`open`；`-10004` 消失。
   - ⚠️ 这一步只证明"能骗过翻译步骤"，**不代表画面会出来**——但它是往下走的门票：
     只有 Unity 真去开文件了，B-1 那个 `clean_jar_path` 的 `gamedata` 映射才开始起作用。
   - 提醒：`build-oddmar-verbose` 才含这个 hook；`build-regress` 里两个字符串都是 0（`grep -c` 自查）。

0-b. 🔴 **顺带做一次"只改 `android_package_code`"的干净 A/B**（与 0-a 的迭代合成一轮）：
   以 `unity.toml.bak-rd9` 为 A（`"./"`），只把 `android_package_code` 改成 `"/game/OddMar/gamedata"`
   为 B，**`android_source_dirs=["./"]` 必须保留**，看 URL host 是否变化。
   ⚠️ 判读前先确认该轮 `init time`=1、`Unable to read header`=0；**出现 `ZZZSENTINEL` / 1 35x 行 /
   `Invalid Reference` 就说明是哨兵态，该轮作废**（§0.6-G 的 args1/args2 就是反例）。

**第 1 组（0-a 通过后再做）**

1. ⚠️ **接线 il2cpp 探针**（§0.5-D 的四步改法），打印 `Application.dataPath` / `streamingAssetsPath` 真值。
   它比上一场更值钱了：host 的来源已排除 obb 一支，现在需要直接问 Unity 自己。
   打印为空 ⇒ 修 Java 侧路径供给；打印正确 ⇒ URL 由别的输入拼装，转查 `libunity.so` 的
   `GetDataPath` 链（`0x39f800 → 0x4bfbe8 → 0x4bbe14`）。
2. **顺着 4 个调用点摸清 `0x4c124c` 背后的实体类型**（`[a0+0x410]` → `vtbl[0x138]`）。
   若要长期保留这个 hook，需要确认它是否只服务于视频链（另三个调用点 `0x5452dc` / `0x78b81c` / `0x78d534`）。

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
6. ~处理 P1 里与主循环/资源相关的 `Activity.getObbDir(s)`~ → **已于 2026-09-22 实现**（`android_misc.cpp:175-176`、
   `android_content.cpp:797-826`），但**没解决 host 为空**（§0.6-F 第 1 条）。
   其余的 `Intent.getExtras`、`Bundle.getBoolean`、`Window.getAttributes`、`SurfaceView.*` 仍留在 P1。
7. 画面真正出内容后，再看 `frame.*.gl.ppm` 是否出现非零像素；若仍黑，再查相机/场景/AssetBundle 挂载。
8. 清理 4.2 的诊断日志（`getConstructorID` 的注册表 dump 现在多了一行 `bound ...`，一并清）。
9. **2026-09-22 新增待办**：`javastubs/android_content.cpp:934` 的注释（"getObbDir / getObbDirs -> STUB-MISS path
   returns null"）已与上面 100 行的实现矛盾，顺手改掉（全仓现在只剩这一处过期注释；
   `android.h:979`、`android_content.cpp:789`/`:896` 那三处已随 09-22 改动消失）。
10. **2026-09-22 新增待办**：探针的生命周期管理——`video translate args` 这类一次性诊断在拿到定论后应降级或
    门控。当前它只在 `BD_BYPASS_VIDEO_TRANSLATE` 打开时才走，**符合"默认路径不受影响"的要求，保持这个形态**。


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
| `BD_BYPASS_VIDEO_TRANSLATE=1`（env） | **2026-09-22 新增**：在 `libunity.so + 0x4c124c` 装 detour，跳过 Unity 的视频路径翻译。**默认不装**。⚠️ 当前实现返回 `char*`，契约要求 `bool` + 写 a2/a3 输出槽 → 必然失败，见 §0.6-E |

**自检命令**（确认二进制里真的有你以为的代码 —— 这一条已救过两次）：

```bash
U=/workspace/Bogodroid/build-oddmar-verbose/unityloader
grep -c "bypassing Unity video path" $U    # 2026-09-22 实测：verbose=1 / regress=0
grep -c "video translate args"      $U    # 2026-09-22 实测：verbose=1 / regress=0
grep -c "PATH-PROBE" $U                   # 2026-09-22 实测：verbose=4（在二进制里，但探针仍未接线）
```

> ⚠️ **`grep` 二进制字符串只证明"代码在里面"，不证明"它打过日志"**。
> 本轮就踩了：源码里有 `video translate args`，但最后一次运行用的二进制**还没有**这两行
> → 全仓日志 `grep -c` = 0，白等一轮。**跑完先 `grep -c` 日志，别只 grep 二进制。**

**构建目录速查（2026-09-22 复核）**：

| 目录 | 类型 | 关键开关 | `unityloader` 时间/体积 | 含视频 bypass？ |
|------|------|---------|----------------------|---------------|
| `build-oddmar-verbose` | **2026-09-22 在用** | RelWithDebInfo / LOG=ON / VERBOSE=ON / NON_ZERO=OFF | 09-22 05:12，153 MB | ✅ hook + args 日志 |
| `build-regress` | 回归对照（上一轮在用） | RelWithDebInfo / LOG=ON / VERBOSE=OFF / NON_ZERO=OFF | 09-21 14:31，152 MB | ❌ 两个字符串都是 0 |
| `build-oddmar` | **旧产物，勿用** | Release / LOG=ON | 09-21 07:55，6.3 MB | ❌ |
| `build-oddmar-sym` | 备用 | RelWithDebInfo | 09-21 08:04，144 MB | ❌ |
| `build-anbernic-debug` | 上机调试 | Debug / LOG+TRACE=ON | 09-16 08:33，102 MB | ❌ |
| `build-anbernic-rel` | **发版用** | Release / LOG=OFF | 09-18 07:22，6.2 MB | ❌ |

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

判读四件套（**必须同时看**，否则会像前两轮那样误判）：

```bash
L=/game/Oddmar/log-seq.txt
grep -c "init time" $L                    # 必须 >0，否则这轮没跑到关键点，结论无效
grep -c ZZZSENTINEL $L                    # 必须 =0；非 0 ⇒ toml 是哨兵态，本轮作废（§⛔ 0-1）
grep -c "Unable to read header" $L        # 必须 =0；且 grep -c "BD-ASSETLOC" 应 >0
grep -n "bypassing Unity\|video translate args\|could not translate\|-10004" $L | head
awk '{print $4}' $SEQ_DIR/timeline.txt | sort -u | head   # 全是 192 就是没画面
```

### 9.3 待清理（新会话建议先清，避免误读旧产物）

- **`/game/Oddmar/seq*` 共 50+ 个目录**：`seq`、`seq2`…`seq90`、
  `seqBASE/BASE2/C32/DEEP/FIX/INERT/LATE/PHANTOM/PROXY/RD2/RD3/RD4/READER/REV/SPLASH`、
  `seq-rd5`…`seq-rd9b`，**2026-09-22 又加了** `seq-obb-test`…`seq-obb-test6`、`seq-jar-bypass{,2}`、
  `seq-hook-test{,2,3,4}`、`seq-args1/2/3`。全部是历次运行的截图与 timeline，**没有源头价值**
  （`seq-args3` 想留就留，它是 hook 入参那轮的产物）。
- **`/game/Oddmar/log*.bak` 7 个**（含 11.9 MB 的 `log-seq.seq90.bak`）：
  `log-seq.before90.bak`(12:17)、`log-seq.seq28.bak`(11:33)、`log-seq.seq90.bak`(12:20)、
  `log-seq.seqC32.bak`(12:25)、`log-seq.seqREV.bak`(11:47)、`log.txt`(08:51)、`log-probe.txt`(09:18)。
  其中 **`seqREV.bak` / `seq28.bak` 是 `getConstructorID` 修复的前后对照，`before90.bak` 是回归基线** ——
  确认不再需要再删。
- **2026-09-22 新增日志**（可清理，但 `log-args3.txt` 建议留到 hook 修好为止）：
  `log-obb-test*.txt`、`log-jar-bypass*.txt`、`log-hook-test*.txt`（4 个，每个 4.7–6.3 MB）、
  `log-args1/2/3.txt`、`args1/2/3.out`。
  其中 **`log-args1.txt` / `log-args2.txt` 是"哨兵态必死"的对照证据**，`log-args3.txt` 是 hook 入参证据。
- ⚠️ **`/game/Oddmar/unity.toml.bak-rd9` 不要删**：它是唯一一份正常配置备份。
- ⚠️ **`/game/Oddmar/unity.toml.sentinel-0922` 不要删**（2026-09-22 新增）：哨兵版留档，做 A/B 对照用。
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

> **案例存档说明（2026-09-22）**：原 `docs/CASE_STUDIES.md` 中与 Oddmar 有关的三个小节
> 已并入本文下方 §10–§12（小节标题保留原文的"案例四 / 四·续 / 四·续三"编号，
> 便于与 `.workbuddy/memory/` 里的工作日志逐条对上）。
>
> 同文件其余三个案例 —— **Skul / Maximus2 / FiveHearts —— 随该文件一并删除**，
> 它们的可复用结论保留在 `docs/PORTING_PLAYBOOK.md` §0（止损判据）/§1.2
> （`JNIVM_ENABLE_RETURN_NON_ZERO` 缓存坑）与 `docs/FIVEHEARTS.md` §4.6 B
> （`textureMaxDim` 误缩 RenderTexture）。需要原文时用
> `git show eb68cf7:docs/CASE_STUDIES.md`（注意：不含末次未提交的「续三」一节），
> 或找回删除前的回收站副本。

---

## 10. 案例 A（原 CASE_STUDIES 案例四）：Oddmar（Unity 2018.4.36f1 / arm64 / Wwise + Firebase）— **启动即退出（进行中）**

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

## 11. 案例 B（原 CASE_STUDIES 案例四·续）：**悬垂的 `jmethodID`** —— 缓存的方法 id 悄悄换了个函数

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


---

## 12. 案例 C（原 CASE_STUDIES 案例四·续三）：**hook 了一个"转发 thunk"** —— 看着触发了，其实恒判失败

> 2026-09-22，Oddmar。这条是**"hook 起效了"与"hook 起作用了"不是一回事**的教科书案例。

### 症状

给 `libunity.so + 0x4c124c` 装 detour，跳过 Unity 的视频路径翻译。日志明确打出：

```
[BD-MEDIA] video translation bypass armed target=0x38004c124c orig=0x37ffff0000
[BD-MEDIA] bypassing Unity video path translation -> /game/Oddmar/gamedata/assets/Videos/…mp4
```

路径字符串**完全正确**、文件**确实存在**（979 746 B），可是 Unity 下一行照旧：

```
Unity: AndroidVideoMedia::OpenExtractor could not translate jar:file://!/assets/Videos/…mp4 to local file.
Unity: AndroidVideoMedia: Error opening extractor: -10004
```

日志里对该文件**零 `stat`/`open`** —— Unity 压根没走到文件层。

### 三件必须先做的事

1. **确认这个地址是"函数入口"还是"函数中段"**：
   `objdump -d libunity.so | grep -nE "(bl|b|cbz|tbnz)[[:space:]]+.*4c124c"`
   —— 有 `bl` 指向它，才是可当函数入口 hook 的。
2. **把它周围的指令读出来**，别只看符号名（stripped 的 so 只有 `JNI_OnUnload+0x…` 这种无意义名）。
   Oddmar 这个 `0x4c124c` 长这样：
   ```asm
   4c124c  mov  x10, x0
   4c1250  ldr  x0, [x10, #0x410]   ; 换成真正实现对象
   4c1254… 参数整体右移一格
   4c1268  ldr  x5, [x11, #312]     ; vtbl[0x138]
   4c1278  br   x5                  ; 尾调用
   ```
   → 这不是"翻译函数本体"，而是**一个虚调用转发 thunk**（`f(a0,a1,a2,a3)` 转发成
   `impl->vtbl[0x138](impl=*(a0+0x410), a0, a1, a2, a3)`）。
3. **回到调用点看返回值怎么被消费** —— 这一步才是决定性的：
   ```asm
   53f140  add  x0, sp, #0x48       ; a0 = 包装对象
   53f144  add  x1, sp, #0x480      ; a1 = 输入路径
   53f148  add  x2, sp, #0x10       ; a2 = 输出槽①
   53f14c  add  x3, sp, #0x18       ; a3 = 输出槽②（相邻 8 B）
   53f150  bl   4c124c
   53f154  tbz  w0, #0, <失败分支>  ; ★ 返回 bool
   53f158  ldp  x8, x3, [sp, #16]   ; 成功后把 (sp+0x10, sp+0x18) 当 {ptr, size} 读出
   ```
   ⇒ 真实契约：`bool f(ctx, in_path, out_ptr, out_len)`，**返回值是 bool，结果走两个输出槽**。
   同时**没有 sret**（调用前 x8 未被设置，thunk 自己拿 x8 当暂存）。

### 为什么"看着触发了"却恒失败

- 我们的 hook `return (uintptr_t)path.c_str();` —— 一个**至少 8 字节对齐**的指针。
  而调用方是 **`tbz w0, #0`**，测的是**第 0 位**。对齐指针低位恒 0 ⇒ **恒判"失败"**。
- 就算把 bit 0 凑成 1 也没用：**两个输出槽从未被写**，调用方会读到 `{null, 陈旧值}`，
  还要拿那个值当长度做边界检查。

### 运行时的独立佐证（很有用的一招）

在 hook 里把 a0–a7 全打出来：

```
args=0x40003a4860d8 0x40003a486510 0x40003a4860a0 0x40003a4860a8
     0x400008d7d5e0 0x17f 0x2f7374657373612f 0x6d2f736f65646956
```

- `a0 - a2 = 0x38`、`a1 - a2 = 0x470`，与 `sp+0x48 / sp+0x480 / sp+0x10` 的间距**逐项吻合**
  → 证实是哪一条调用点、证实 a2/a3 是"相邻 8 字节的两个输出槽"；
- `a6 = "/assets/"`、`a7 = "Videos/m"` 的 ASCII ——**上一层的寄存器残值**，不是参数。

> **判据**：凡是参数打印里出现"可读文本被当成指针"，基本可以断定**该参数位是空的**
> （调用者没设），不要拿它当线索。

### 通用教训

- **hook 成功不等于 ABI 对上。** 装上了、打印了、触发了一次 —— 这些都只证明"跳转发生了"。
  真正要证的是**返回值/输出参数的约定**。
- **`tbz w0, #0` / `tbnz` 是"测某一位"，不是"测非零"。** 任何"返回指针去满足返回 bool 的接口"
  的写法都必然为假，因为对齐指针低位是 0。看到 `tbz/tbnz w0, #0` 就要立刻想到：
  这个函数的返回值是**布尔**。
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
- `.workbuddy/memory/2026-09-22.md` —— 第三场工作日志（本文件 §0.6 的原始记录）

**版本**：本文件 2026-09-22 第三场更新（新增 §0.6、改写 §⛔ 0-1 / §0 / §7.0 / §8 / §9.2-9.3）。
上一版是 2026-09-21 第二场。
2026-09-22 同日晚：并入原 `docs/CASE_STUDIES.md` 的 Oddmar 三节（现 §10–§12），该文件已删除，原「§10 相关文档」顺延为 §13。
