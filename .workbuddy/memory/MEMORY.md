# Bogodroid-anbernic — 项目长期记忆

## 项目是什么

把 Android 端 Unity IL2CPP（arm64）原生库包进 ARM Linux 掌机运行
（`gmloader-net` + `libjnivm`）。每个游戏 = `projects/<name>/` + 共享 `libjnivm/`、
`thunks/`、`platform/`、根 `javastubs/`。

仓库目录 ↔ 分支（2026-09-22 起重排）：

| 目录 | 分支 | 用途 |
|------|------|------|
| `D:/Locke/gitee/Bogodroid` | `unity6` | Unity 6 线 |
| `D:/Locke/gitee/Bogodroid-oddmar` | **`oddmar`** | **本目录 —— Oddmar 的活在这里干** |
| `D:/Locke/gitee/Bogodroid-anbernic` | `video` | 2026-09-22 从 oddmar 让回 video |

> ⚠️ `Bogodroid-anbernic` 那个目录**有外部工具/IDE 在按分支差异做"删了再写"**。2026-09-22 两次事故：
> 15:11 整个 `docs/`、15:39 **28 个跟踪文件**（`scripts/`、`javastubs/` 部分、`AGENTS.md`…）被移进
> 回收站，且**只删没写回**。⇒ 在那边切分支前先清工作区，切完立刻 `git status` 复核；
> 丢了用 `git checkout -- .` 即可复原（内容都在 git 里）。回收站 `/d/$Recycle.Bin/<SID>/` 是最后一道保险。

## 构建 / 测试环境（硬性）

- **只用 `GlES_Dev` 容器**（`dropbeak-gles-dev:local`，aarch64 Ubuntu 20.04 + g++ 9.4）。
  `dropbeak-gles-dev:local` **镜像里没有** g++/cmake/ninja，工具链只在该容器可写层。
- 源码用 `docker cp` 单文件同步进 `/workspace/Bogodroid`，不要整树重推。
  **`docker cp` 保留 mtime → ninja 会说 "no work to do"，改完必须 `touch`。**
  ⚠️ **主机侧路径在 Git Bash 里会被错解**：`/d/Locke/...` → `d:\d\Locke\...`（`CreateFile` 报错）。
  主机侧改用 PowerShell 工具传 `D:\...` 原生路径；容器侧 `/path` 不受影响。
- 每次配置显式带 `-DJNIVM_ENABLE_RETURN_NON_ZERO=OFF`（CMake 缓存项，共用目录会被上次实验污染）。
- 构建目录分工：
  - `build-oddmar-verbose` — RelWithDebInfo、无 LTO、`-g`、LOG+VERBOSE 开（迭代诊断，≈2–4 min）
  - `build-oddmar` — Release + LOG=ON + TRACE/VERBOSE OFF（上机中间态）
  - `build-regress` — RelWithDebInfo + LOG=ON + TRACE/VERBOSE OFF（回归对照用）
- 运行脚本固定要显式传 `BOOT_LOADER=`，否则默认跑 `/workspace/Bogodroid/build-oddmar`（老二进制）。
- 本机 Bash 需 `export PATH="/usr/bin:/bin:/c/Windows/System32:/c/Users/Administrator/.workbuddy/binaries/PortableGit/versions/1.2.0/usr/bin:$PATH"`。

## 容器状态陷阱（2026-09-22 定案）

- **`/game/Oddmar/unity.toml` 会被留成"哨兵"态**（`android_package_code="/tmp/ZZZSENTINELPACK.apk"`
  + `android_source_dirs=[]`）。哨兵态必死：日志只有约 **1 35x 行**（正常轮 ≈17.7 万行）、
  `ZZZSENTINEL` 命中十几次、`Invalid Reference, Unexpected Type`（Unity 弹错误对话框 → JBRIDGE 代理类型不匹配）。
  **开工第一件事就是 `grep` 内容核对**（两个 toml 的 mtime 一样，别看 mtime）；还原用
  `cp /game/Oddmar/unity.toml.bak-rd9 /game/Oddmar/unity.toml`。哨兵版留档 `unity.toml.sentinel-0922`。
- **判读任何一轮测试，几个计数一起看**：`ZZZSENTINEL`=0、`Unable to read header`=0、
  `could not translate`=0、`seq/timeline.txt` 里不止 192 B（192 B = 纯色 = 没画面）。
  少看一个就会得出反向结论。
  **⚠️ `init time` 不可单独判死**：它读的是 Unity 自报的初始化耗时；2026-09-22 晚**成功轮也是 0**
  （toml 用 `./` 本地目录态时该值本就为 0，正常轮日志首行即 `Unity: init time: 0`）。
  判活靠 `ZZZSENTINEL`=0 + 画面字节数，别把 `init time`=0 当成失败信号。

## hook 一个 stripped 的 `.so` 之前（2026-09-22 教训）

先读三处汇编，再动手：① 目标地址是不是函数入口（有 `bl` 指向它）；② 入口序列的形状
（`ldr x0,[x0,#N]` + `br` = 转发 thunk，不是本体）；③ **调用点怎么消费返回值**。
`tbz/tbnz w0, #0` = 返回值是 **bool**（测第 0 位），**对齐指针低位恒 0 → 恒判失败**。
调用前没设 x8 就**不是 sret**。

⚠️ **2026-09-22 晚补强：只读"调用点怎么消费返回值"仍然不够 —— 必须读完整个消费者函数（prologue→ret）。**
Oddmar 那次就是只看了 `0x53f150` 附近，把 `a1` 当"只读输入路径"、把 `a2/a3` 当两个输出槽；
读完消费者 `0x53f020` 才发现 `a1` 是**输入/输出的 string**（调用前被清空，必须回填本地路径）、
`a2 = NULL` 才是正解（非 0 会被当成文件偏移）、`a3` 是数据总长（也是 `offset+size` 边界检查的上界）。
**"能走到下一行"不等于参数对上了** —— 中途两版都走到了 `open` 且 `fd=30` 成功，**依然是错的**。
详见 `docs/ODDMAR.md` §0.6-C/D/E（四次迭代失败史）。

> 原 `docs/CASE_STUDIES.md` 已于 2026-09-22 删除：Oddmar 三节并入 `docs/ODDMAR.md`
> §10–§12，Skul / Maximus2 / FiveHearts 随文件丢弃（结论保留在 `docs/PORTING_PLAYBOOK.md`
> §0 / §1.2 与 `docs/FIVEHEARTS.md` §4.6 B）。文档引用一律指向 `docs/ODDMAR.md`
> （原名 `docs/HANDOFF-ODDMAR.md`，2026-09-22 收尾时改名）。

## 定位 libunity 私有全局 / 虚函数来源（2026-09-22 深夜新增，通用手法）

前置事实：**这个 build 的 `vaddr == file offset`**（`.text`/`.rodata` 都是）⇒
`strings -t x` 给出的偏移**可以直接当虚拟地址**用。四步：

1. **字面量定位**：`strings -t x lib.so | grep <关键串>` 拿地址，再用 python 转储该地址前后 ~0x120 B，
   看它落在哪一簇（**同名类名/协议名成簇出现**是关键线索 —— 本 case `FileSystemAndroidAPK`、
   `AndroidSplitFile`、`LocalFileSystemAndroid` 和 `jar:file://` 挤在一起，直接指向文件系统类族）。
2. **全量反汇编 + adrp 配对 xref**：`objdump -d --no-show-raw-insn lib.so > uni.asm`
   （15 MB 的 so → 96 MB asm，约 26 s）。AArch64 引用字符串 = `adrp xN,<page>` + `add xN,xN,#off`；
   ⚠️ **objdump 的立即数可能是十进制**（`#3080` 就是 0xC08），配对正则要同时吃 `0x…` 与纯十进制。
   写个扫描器按寄存器记住最近一次 `adrp`，再在 `add`/`ldr`/`str` 上配对。
3. **虚函数槽解析**（`vtbl[0x170]` 这类**只有运行时才知道 vtable 是谁的**）：
   在 loader 里加一个诊断，打印 `*(base+全局偏移) → +N → vtbl → [slot]` 并输出成 `lib.so+偏移`，
   再去反汇编那个函数（本 case 一读就明白了：`return std::string(g_appPathBuf)`）。
   想静态交叉验证：`readelf -rW` 取 `R_AARCH64_RELATIVE`，`(reloc_offset, addend)` 排序找连续 run，
   `run_base + slot_off` 即槽值 —— 本 case 静态推的 `base=0xe5c068`、槽 `0x30c704` 与运行时**逐位一致**。
4. **GOT → 真实对象**：若槽值落在 `.got`，读该地址的重定位 —— `R_AARCH64_RELATIVE` 的 addend
   就是它指向的本模块地址（本 case `0xE8BC08 → 0xF0B740`，一个 `.bss` 静态缓冲）。
   再反向扫"**谁写这个地址**"（adrp+add/str 配对）就拿到上游 setter。

**判据小结**：`stat()` / `S_IFREG` 这类"**路径存在性闸门**"会让 port 自造的
"逻辑路径"**静默作废**。日志里 `[NATIVE] stat(<你刚返回的路径>)` **紧跟**在 JNI 返回值之后，
就是它 —— 本 case 正是 `getPackageCodePath -> …apk` 的下一行。

## 日志规则（踩过的大坑）

- `jnivm_log_enabled(tag, format)` 看的是 **tag** 是否以 `BD-` 开头，**不是** format。
  自定义诊断一律 `LOG("BD-XXX", ...)`，否则被静默丢弃。
- 全关（`BD_ENABLE_LOG=OFF`）只用于压体积/对照性能；深挖再临时开 TRACE/VERBOSE。
- `fatal_error` / SEGV 回溯**不依赖** `BD_ENABLE_LOG`。

## Oddmar（当前在手）

- **目标：断网单机运行**（不接 Google Play / 内购 / 在线服务）。
  billing、Firebase、Play Services 类缺失桩**不需要功能正确**，只要不阻塞、不致命。
- 排查顺序固定：**资源加载 → 场景挂载 → 画面**。
- **✅ 2026-09-22 晚：黑屏已解决** —— 画面 = Oddmar 标题界面（X 根窗口截图稳定 290–300 KB）。
  关键契约：`libunity+0x4c124c` 是 asset 路径翻译（转发 thunk），
  `bool f(obj, string* path_in_out, void** out_base, size_t* out_len)`；
  **a1 装原始 URL、调用前被清空、必须回填本地路径；a2 必须 NULL；a3 = 数据总长**。
  实现 `projects/unityloader/main.cpp` 的 `bypass_video_translate`，env `BD_BYPASS_VIDEO_TRANSLATE=1` 才装。
  详见 ODDMAR.md §0.6-E（实现）/ §0.6-H（实测结果）。
- **✅ 2026-09-22 深夜：L2 已定位** —— hostless URL 的 host = **`Application.dataPath`**。
  上游 = `Context.getPackageCodePath()`（port 侧 `bd_compute_source_dir()`），
  被 Unity 的 **`stat()` + `S_IFREG` 闸门**挡下（返回的 `<cwd>/UnityDataAssetPack.apk` 在磁盘上不存在）
  ⇒ 候选作废 ⇒ `dataPath=""` ⇒ `streamingAssetsPath = "jar:file://!/assets"`。
  链条：`0x47e940` 拼 `"jar:file://" + dataPath + "!/assets"`（对 getter 无校验）；
  dataPath 出自静态缓冲 `libunity+0xF0B740`，getter `0x39f800`→`vtbl[0x170]`=`0x30c704`，
  setter `0x39f818` ← 唯一调用者 `0x31c520`，闸门 `0x31c56c`（stat + S_IFREG）。
  探针 `BD_PROBE_APPPATHS=1` **已接线**（`[BD-PATH-PROBE]` / `[BD-L2]`）。
  **根治不建议做**（要放占位 APK + 动跨端口共享的 `clean_jar_path`）。详见 ODDMAR.md §0.7。
  ⚠️ **别在 `gamedata/` 留 `UnityDataAssetPack.apk`** —— 留了 `dataPath` 非空，Unity 转去 APK 内部找
  数据档 ⇒ 每轮必死（ODDMAR.md §⛔ 0-5）。
- 交接文档：`docs/ODDMAR.md`（先读它）。
- 资源 staging：`D:\Locke\gitee\LinuxArmPorts\oddmar_port_stage\`。

## 回归测试素材

`D:\Locke\gitee\LinuxArmPorts\` 下的发布包（自带 `unityloader` + `gamedata/`，可直接跑）：
- `武士2复仇.zip` → `Samurai2/`（依赖插件 `unityloader.d/samurai2_offline.so`，`[google_play] offline=true`）
- `街头角斗士2.zip` → `Maximus2/`（Unity 2020.3 + `-force-gles32`）

包自带 loader 是 **Release + LOG=OFF + strip** 的上机版，可作"改动前"基线。
容器内通用跑测脚本：`/regress-run.sh`（`GAME_DIR=… LOADER=… LABEL=… SECS=… bash /regress-run.sh`）。

## 工作方式偏好

- 结论前置；证据要能复现（命令 + 现象）。
- 不猜；没跑通的不说"完成"。
- 破坏性/对外动作先确认。本地树改动前先 `git status` 看清。
