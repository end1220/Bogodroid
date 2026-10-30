# Unity IL2CPP → Linux ARM 掌机移植 Playbook

面向 1GB 级 UMA 掌机（如 Anbernic BuildRoot）。**新端口先读本文。**

**这是唯一的通用流程文档。** 三层分工：

| 层 | 文档 | 放什么 |
|---|---|---|
| 决策 + 执行 | **本文** | 该不该做、止损判据、六段流水线、命令、断言、硬坑、维护契约 |
| 实例 | `docs/<端口名>.md`（如 [`BRICKGAMEPRO.md`](BRICKGAMEPRO.md)、[`BLACKMOOR2.md`](BLACKMOOR2.md)） | 这款游戏的具体参数、专属复测项、踩的坑、进度 |
| 专题 | [`UNITY6.md`](UNITY6.md)、[`HAPTICS.md`](HAPTICS.md) 等 | 单点深入 |

**本文只放"照着做就对"的规范，不放"某人某天做到哪"的过程。** 过程进实例文档。
下文用 `<PORT>` / `<port_id>` 占位，对照表见 §1.4.1。

---

## 0. 30 秒速读

### 0.1 你的处境读哪一节

| 处境 | 读哪 |
|---|---|
| 判断这个游戏值不值得做 | §1 止损判据 → §5 选游戏与引擎初判 |
| 手上游戏已确定，要跑通 | §1 六段流水线，从头顺着走 |
| 跑到一半卡住 | §0.4 基线状态 → §0.5 症状定位 |
| 只改一个按钮 / 一个画面参数 | §4 插件约定 → §1.6 容器回放，别从头读 |
| 目标是 Unity 6000.x | 先 [`UNITY6.md`](UNITY6.md)，再回本文 |
| 大文件推送上去就残 | 根目录 [`AGENTS.md`](../AGENTS.md) 的 Dropbeak 段 |

### 0.2 六段流水线（顺序不能跳）

```text
解包落位 → 资产处理 → 构建 → 容器回放取证 → 真机确认 → 打包发版
```

两条硬规则：

1. **游戏专属逻辑只进插件** `projects/unityloader/plugins/<port_id>/`，共享 loader 里不加游戏名分支。
2. **行为改动先在容器回放里拿到可复现证据**（截图 + 日志 + 断言脚本），再上真机。真机只做"确认"，不做"试"。

### 0.3 三条命令（最常打的三条，可直接粘）

> 前置：①② 都在 **`GlES_Dev` 容器内 `/workspace/Bogodroid`** 下跑；容器没有 bind mount，改完源码要先 `docker cp` 进去（见 §1.5）。

```bash
# ① 排查构建（带 LOG，跑得动）。发版用全关版，见 §1.1
docker exec GlES_Dev bash -lc 'cd /workspace/Bogodroid &&
  cmake -S . -B build-anbernic-rel -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBD_ENABLE_LOG=ON -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF \
    -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF &&
  cmake --build build-anbernic-rel -j"$(nproc)" --target unityloader'

# ② 容器回放取证。参数全表见 §1.6.2
env GAME_DIR=/game/<PORT> W=640 H=480 SECS=41 DUMP_AT=180 CAP_MS=200 \
    BD_PAD_REPLAY='15000:start,18000:a' BD_PAD_REPLAY_HOLD=600 \
    SEQ_DIR=/game/<PORT>/seq-ui-exit \
    bash scripts/container-run-game-seq.sh

# ③ 断言，不看图。退出码非 0 就是没过
python scripts/verify-<port_id>-ui.py deploy/<PORT>/ui-validation --case exit
```

顺序是**②③ 通过才允许 ④ 真机**。反过来做（先上真机再回来补证据）是这类移植最常见的返工原因。

### 0.4 基线状态（唯一走完全流程的样本）

| 环节 | 状态 | 证据位置 |
|---|---|---|
| 解包 / 资产 / 构建 | ✅ | 见实例文档 |
| 容器回放 | ✅ | `deploy/BrickGamePro/ui-validation/`（含 `runner.txt`） |
| 真机确认 | ✅ | TrimUI / Anbernic，端点表见 §1.7.1 |
| 打包发版 | ✅ | `docs/BRICKGAMEPRO.md` 的 SHA256 表 |

**唯一完成全部六段的样本：BrickGamePro（`FatFishBrick`）。**
Blackmoor 2 走到第四段，卡在手柄输入，见 [`BLACKMOOR2.md`](BLACKMOOR2.md)。

**怎么判断本文有没有过期**（动手前先跑，别信表里写什么）：

```bash
git log -1 --format='文档: %cd' -- docs/PORTING_PLAYBOOK.md
git log -1 --format='共享层: %cd' -- thunks/ javastubs/ loader/ projects/unityloader/
```

判读：

- 后者比前者新 → 共享层动过，上表的 ✅ 不再代表当前代码，按 §7 复测清单重跑再信。
- 第一条输出为空 → 文档还没进 git，没有基线时间，只能当草稿。

### 0.5 卡住时按症状定位

| 症状 | 先看 | 一句话判据 |
|---|---|---|
| `exited 134` / `Invalid Reference` / 崩在无关桩上 | §1.2、§1.3 | CMake **缓存**遗留，不是代码 bug |
| 进程活着但画面全黑 | §1.6.2 末段 | png 黑 ≠ 没画，先看 `frame.*.gl.ppm` 和 `timeline.txt` |
| Unity 报 `initJni not found` | [`UNITY6.md`](UNITY6.md) | 类搬家了，不是符号缺失 |
| 事件注入了 `result=1` 但游戏不动 | §8 硬坑清单 | 设备枚举 / 签名擦除 / KeyEvent 翻译层三选一 |
| 掌机推送超时或文件残缺 | §1.7.2 | 必须带 `--force --chunk --verify` 并核对大小 |
| `AssertionError` 但图看着正常 | §1.6.4 | 脚本做的是断言不是打印，看图不算数 |

---

## 1. 标准流水线

```text
解包落位 → 资产处理
    → 构建（Release + 中间态日志，编 unityloader + plugin）
    → 容器回放取证（Xvfb + 软件 GL + 按键回放 + 断言）
    → 真机确认（Dropbeak push --force --chunk --chunk-size 16m --verify → Ports 手启）
    → 打包发版（package-handheld.py → tar + SHA256SUMS）
```

真机只做"确认"，不做"试"。凡是要在掌机上反复试效果的东西，都先在 §1.6 的容器回放里跑。

### 1.1 发布构建

两种档位、开关口径**只在 §6**，这里只给命令。日志是编译期开关（`platform/common/logging.h`），改 toml 不会多一行。

构建在 `GlES_Dev` 容器里做，源码副本是 `/workspace/Bogodroid`（**不是 bind mount**，宿主改完要 `docker cp` 进去）。容器基线、同步细节、产物检查见 [`../BUILD-DOCKER.md`](../BUILD-DOCKER.md)。

发版（LOG 全 OFF + strip，约 5.7 MB）：

```bash
docker exec GlES_Dev bash -lc '
  cd /workspace/Bogodroid &&
  cmake -S . -B build-device-release -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBD_ENABLE_LOG=OFF -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF \
    -DIL2CPP_TRACE=OFF -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF &&
  cmake --build build-device-release -j"$(nproc)" --target unityloader plugin_<port_id> &&
  aarch64-linux-gnu-strip build-device-release/unityloader \
    build-device-release/unityloader.d/<port_id>.so
'
```

排查（LOG=ON、不 strip，约 90–168 MB，只临时上机）：

```bash
docker exec GlES_Dev bash -lc 'cd /workspace/Bogodroid &&
  cmake -S . -B build-anbernic-rel -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBD_ENABLE_LOG=ON -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF \
    -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF &&
  cmake --build build-anbernic-rel -j"$(nproc)" --target unityloader'
```

发版与排查用**两个构建目录**：`cmake -S . -B` 会改写 `CMakeCache.txt`，共用目录会互相污染档位（实测 `build-device-release` 里就留着一次 `BD_ENABLE_LOG=ON`）。

插件有两个等价 target 名：`plugin_<port_id>`（`plugins/CMakeLists.txt:8` 的 `add_custom_target`）与 `bogodroid_unityloader_plugin_<port_id>`（同文件 `:6` 的库 target），`--target` 用哪个都行。**产物文件名是 `<port_id>.so`**（`OUTPUT_NAME` + `PREFIX ""`），落在 `unityloader.d/`。全部插件一起编用 `unityloader_plugins`。

`scripts/build-docker.sh` 与 `scripts/unity6/build-{rel,log}.sh` 是上游 x86 Debian 交叉路线，**已废弃**
（镜像缺 FFmpeg dev、Debian 11 源 404），不要照它们编——三个脚本现在都在开头直接报废弃并 `exit 1`。

原先封着排查档参数的 `scripts/build-anbernic.sh` **已删除**：它写死 `BD_ENABLE_OPENSLES_SHIM=OFF`，
而该选项默认早已是 ON，照它编出来的包在 Wwise 端口上启动即 `SIGABRT`。所以这里改成贴完整命令，
flag 由本文档负责，不再有第二份真相。

### 1.2 `JNIVM_ENABLE_RETURN_NON_ZERO`：必须显式 OFF

`libjnivm` 的这个选项决定**缺桩**（日志里的 `[STUB-MISS]`）返回什么：

| 值 | 缺桩返回 | 后果 |
|----|----------|------|
| **OFF**（默认，上机必须） | `null` / 0 | Unity 自己 try/catch 掉，最好情况只丢一个 NRE |
| ON（实验） | 硬造一个 dummy 对象 | Unity 把它 cast 成 `String` / `Throwable` → jnivm `Invalid Reference, Unexpected Type` → `terminate()` / `exited 134` |

它是 **CMake 缓存项**，不是源码常量：共用 `build-aarch64/` 时，只要有一次实验把它设成 `ON`，之后即使只改无关代码，编出来的 `unityloader` 也会崩，且崩点看起来发生在"完全不相干"的桩上（实测踩过：`File.getParent`、`Context.getContentResolver`、`java/lang/Error`）。

**所以每次构建都显式写 `-DJNIVM_ENABLE_RETURN_NON_ZERO=OFF`**，编完核对：

```powershell
Select-String -Path build-aarch64\CMakeCache.txt -Pattern JNIVM_ENABLE_RETURN_NON_ZERO
```

（Ninja 构建下想再确认编译命令里没带宏，可在容器里 `ninja -t commands | grep -c JNI_RETURN_NON_ZERO`，应为 `0`。）

判读崩溃日志时先分清：`terminate called recursively` / `exited (134)` + `Invalid Reference, Unexpected Type` = 本开关被打开的症状，**不要**顺着最后一个 `STUB-MISS` 去补桩；`[BD-SEGV]` + libunity 地址才是游戏内部异常。Maximus2 就是踩这个坑的现场：它是在"只加了分辨率探测"的构建上开始崩的，三次日志的最后一个 `[STUB-MISS]` 各不相同，照着补桩全部无效。

### 1.3 其它 `JNIVM_*` 缓存项：`FORCE` 与各自的正确取值

`JNIVM_ENABLE_RETURN_NON_ZERO` 不是孤例：`libjnivm` 的开关全是 **CMake 缓存项**，`set(... CACHE BOOL ...)` 不带 `FORCE` 时，上一次配置遗留的旧值会静默存活（这正是 §1.2 那类崩溃的来源）。`CMakeLists.txt` 现在把这两项 `FORCE` 成固定值：

| 开关 | 取值 | 原因 |
|------|------|------|
| `JNIVM_ENABLE_DEBUG` | **恒 ON** | **不是日志开关**。它即 `JNI_DEBUG`：包住 `InternalFindClass()` 里的命名空间/类注册遍历（影响嵌套类如 `Foo$Bar` 的类身份与 `nativeprefix`），并保留 `CallMethod/GetField object is null` 诊断。所有上机验证过的构建都是 ON，未上机验证前别改 |
| `JNIVM_ENABLE_TRACE` | 跟随 `BD_ENABLE_LOG` | 即 `JNI_TRACE`，只加 `LOG()`。但 `BD_ENABLE_LOG=OFF` 时这些块里除了死日志，还有**未知字段路径**上的 `Util::GetClass()`（非日志调用，每次都会执行），所以 LOG 关时一并关掉 |

核对缓存（`FORCE` 后应等于上表）：

```powershell
Select-String -Path build-aarch64\CMakeCache.txt -Pattern 'JNIVM_ENABLE|BD_ENABLE_LOG'
```

一个容易踩的交互：`cmake --build` 发现 `CMakeLists.txt` 比 `build.ninja` 新时会**自己重跑 configure**，用的是缓存里的值。所以"只改 CMakeLists、命令里不带 `-D`"得到的是**旧缓存 + 新建模规则**的混合结果；要确定，先显式跑一次 §1.1 的 `cmake -S . -B build-aarch64 -D...`，或删掉 build 目录重配。

### 1.4 运行时目录布局

#### 1.4.1 占位符对照

本文 `<>` 占位符对应的实际值：

| 占位符 | BrickGame 实值 | 说明 |
|---|---|---|
| `<PORT>` | `FatFishBrick` | 掌机菜单名 / 运行目录名 |
| `<port_id>` | `brickgamepro` | 插件与配置身份（构建 target、toml、.so 名） |
| `<GAME_DIR>` | `/game/BrickGamePro` | 容器内 staging 目录 |
| `<HOST_PORT>` | `/mnt/mmc/Roms/ports/FatFishBrick` | 掌机运行目录 |

新端口在实例文档里建一张同构的表，别散落在正文。

#### 1.4.2 目录结构（所有端口一致）

`deploy/<PORT>/` 是本地 staging（git 忽略），结构**就是**掌机上的最终结构：

```text
<PORT>/
├── <PORT>.sh                    ← scripts/<port_id>_launch.sh 部署过来
├── unityloader                  ← 构建产物
├── unity.toml                   ← configs/<port_id>.toml 部署过来
├── gamecontrollerdb.txt
├── unityloader.d/
│   └── <port_id>.so             ← 构建产物（插件）
├── game/                        ← init_config 会 chdir 到这里
│   ├── lib/arm64-v8a/*.so       ← libunity / libil2cpp / libc++_shared …
│   ├── assets/bin/Data/         ← 含 Managed/Metadata/global-metadata.dat
│   └── res/drawable/splashvideo.mp4
├── conf/                        ← android_files / android_external_files
└── cache/                       ← android_cache
```

**路径坑（必记）**：toml 里 `android_files="../conf/"`、`android_obb_dirs=["./"]` 之所以长得绕，
是因为 **cwd 已经被切到 `game/`**。改配置时先记住这一点，否则会把路径接成 `game/game/`。

`[device]` 三项保持 `0`：分辨率在运行时由 SDL → `/dev/fb0` 探测，同一份 TOML 通吃 640×480 和 1280×720。

### 1.5 排查构建（带 LOG，跑得动）

在 `GlES_Dev` 容器内、`/workspace/Bogodroid` 里跑：

```bash
docker exec GlES_Dev bash -lc 'cd /workspace/Bogodroid &&
  cmake -S . -B build-anbernic-rel -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBD_ENABLE_LOG=ON -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF \
    -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF &&
  cmake --build build-anbernic-rel -j"$(nproc)" --target unityloader'
```

用 `RelWithDebInfo` 而不用 `Debug`：`-O0` 下帧率会失真，"视频卡"这类判断在 Debug 构建上没有意义。
`RelWithDebInfo` 是 `-O2 -g`，跑速接近 `Release`，回溯可读；**不要 strip**。

其余选项一律走默认值（`USE_MOLD=OFF`、`BD_FAKE_EGL=ON`、`BD_ENABLE_OPENSLES_SHIM=ON` 默认就是要的）。
**唯一不能碰的是 `BD_ENABLE_OPENSLES_SHIM`**：写成 `OFF` 的包在 Wwise 端口上启动即
`Unknown symbol "slCreateEngine"` + `SIGABRT`（出处见 [`ODDMAR.md`](ODDMAR.md) §0.13e 与 §8 硬坑表）。

`GlES_Dev` **没有 bind mount**，改完源码必须 `docker cp` 进容器（`docker cp` 保留 mtime，必要时 `touch`），否则编的还是旧文件。

两个构建目录分开用：`build-docker`（正式）/ `build-anbernic-rel`（排查）。混用会导致"发版包里混进日志"或"排查时没有日志"。

### 1.6 容器回放取证（本流程的核心环节）

不碰真机就能把交互流程跑一遍。组成：Xvfb + 软件 GL + 脚本化按键回放 + 帧导出 + 断言脚本。

#### 1.6.1 跑一次

```bash
env BRICKGAME_PROBE_START=1 \
    BD_PAD_REPLAY='15000:start,18000:a,21000:guide' \
    BD_PAD_REPLAY_HOLD=600 \
    GAME_DIR=/game/<PORT> W=640 H=480 SECS=41 \
    DUMP_AT=180 CAP_MS=200 \
    SEQ_DIR=/game/<PORT>/seq-ui-exit \
    bash /workspace/Bogodroid/scripts/container-run-game-seq.sh
```

#### 1.6.2 参数表

| 变量 | 含义 |
|---|---|
| `GAME_DIR` | staging 目录，日志落到 `$GAME_DIR/log-seq.txt` |
| `LOADER` | 指定 unityloader 路径，默认取 `GAME_DIR/unityloader` |
| `W` / `H` | 窗口尺寸，掌机两种都要覆盖（640×480 / 1280×720） |
| `BD_PAD_REPLAY` | `毫秒:按键` 序列，脚本化按键注入 |
| `BD_PAD_REPLAY_HOLD` | 每键按住时长（ms） |
| `BD_PAD_AXIS_REPLAY` | `毫秒:轴名:值` 序列，走 MotionEvent 轴路径 |
| `DUMP_AT` | 从第几个 swap 开始导帧（软件 GL 约 25 swap/s，180 ≈ 7 s） |
| `CAP_MS` | 截图间隔 |
| `SECS` | 总运行时长，到点收工 |
| `SEQ_DIR` | 产物目录，先清空再写 |

`BD_DUMP_FRAME` 由脚本自动设上。产物：`%04d.png`（root window）、`frame.<n>.gl.ppm`（glReadPixels）、
`timeline.txt`、`xwininfo.txt`，日志落 `GAME_DIR/log-seq.txt`。

**png 是黑的不能直接判死**——先看 ppm 里有没有内容，再看 `timeline.txt`，把"没画"和"没抓到"分开。
反过来也成立：`glReadPixels` 的 ppm 可能全黑（读了 swap 之后的 back buffer），而 X 根窗口截图正常。
判画面**一律看 png**，ppm 只当辅助。

**抓帧时机**：冷启动到首帧可能要 30 s 以上（软件 GL + 大包），`SECS`/`DUMP_AT` 给短了只会抓到黑屏。
先用一次长跑 + `timeline.txt` 找出"首帧在几秒"，再按那个时间点定参数。

#### 1.6.3 用例矩阵

```bash
bash scripts/container-<port_id>-ui-matrix.sh          # 全部：finish / buttons / exit
bash scripts/container-<port_id>-ui-matrix.sh exit     # 单个
```

同族还有 `-down-matrix.sh`（下落节奏）、`-rotate-matrix.sh`（旋转）、`-tank-speed.sh`、
`container-unityloader-power-standby.sh`（电源键待机）。

#### 1.6.4 用断言脚本判读，别肉眼看图

```bash
python scripts/verify-<port_id>-ui.py deploy/<PORT>/ui-validation --case exit
```

这些脚本做的是**断言**，不是打印。BrickGame 的 UI 校验要求日志里必须有
`viaGUI=1`、`Finish icon GameOver installed=1`、`UI hidden renderers>=19 badAlpha=0`，
且**不得出现** `[BD-SEGV]` / `terminate called` / `Invalid Reference` / `fatal error`。
断言不过就是没过，不存在"看着差不多"。

#### 1.6.5 证据留档

每次验证新建目录，命名 `<主题>-validation/`：

```text
deploy/<PORT>/ui-validation/
├── seq-ui-finish/  seq-ui-buttons/  seq-ui-exit/
│   ├── 0001.png … (截图)
│   ├── log.txt      (log-seq.txt 拷贝)
│   └── runner.txt   (回放命令与环境)
```

`deploy/` 已被 git 忽略，这些是本地证据，不进提交。

### 1.7 真机确认

#### 1.7.1 端点

| 设备 | 地址 | 运行目录 |
|---|---|---|
| Anbernic | `172.16.7.42` | `/mnt/mmc/Roms/ports/<PORT>` |
| TrimUI | `172.16.6.55` | `/mnt/SDCARD/Data/ports/<PORT>` |

（`172.16.6.250`、`172.16.7.55` 是历史地址。）

设备基线（真机验证过的部分）：

| 设备 | 面板 / GPU | 系统 SDL | 界面 |
|---|---|---|---|
| Anbernic H700 | 640×480、Mali-G31、GLES 3.2 | 2.0.12（构建容器 `pkg-config` 报 2.0.10，同代；**链系统这份，不要 bundled 新 SDL**） | `dmenu` / `launcher.sh`，**不是** Weston |

启动方式两家不同，别套用：

- **Anbernic**：只能从 Ports 菜单点启动——`dmenu` 占着 framebuffer，Dropbeak `exec` 拉游戏会失败。卡分区是 `noexec` 时先 `mount -o remount,exec /mnt/mmc`，别把 loader 拷到 `/tmp` 绕（会走 KMSDRM 跟菜单抢屏）。
- **TrimUI**：可以远程拉起，见根目录 [`AGENTS.md`](../AGENTS.md) 的 `runtrimui.sh` 段。

#### 1.7.2 推送

只推变动的三个文件——loader、插件、配置——资源不动：

```bash
dropbeak-cli push build-docker/unityloader \
  <HOST_PORT>/unityloader \
  --force --chunk --chunk-size 16m --verify

dropbeak-cli push build-docker/unityloader.d/<port_id>.so \
  <HOST_PORT>/unityloader.d/<port_id>.so \
  --force --chunk --chunk-size 16m --verify
```

推前把旧的挪到 `backup-<日期>/`（放 `unityloader.d/` 外面），别直接覆盖。
推完核对字节数和 SHA256，**一次超时就当残缺文件整份重推，不做续传**。

#### 1.7.3 启动脚本

走菜单里的 `<PORT>.sh`。脚本做的事：`cd` 到自身目录、`require_file` 逐个校验关键文件、
建 `conf/` 和 `cache/`、按是否存在 pulseaudio 选 `SDL_AUDIODRIVER`、
设 `MALLOC_ARENA_MAX=2` 等内存参数、最后 **前台 `exec`** loader。

**`exec` 不能改成后台**，否则 launcher 立刻返回，固件会把主界面拉回来，表现成界面和游戏来回闪。

#### 1.7.4 完整部署校验

```bash
python deploy/<PORT>/inspect-startup.py    # 启动时序
python deploy/<PORT>/inspect-exit.py       # 退出流程
python deploy/<PORT>/inspect-fonts.py      # 字体
```

历史记录里 BrickGame 一次完整部署是 **144 / 145 个运行时文件全过 SHA-256**。
这个数字是验收线：不是"推上去了"，是"逐个哈希对上了"。

### 1.8 打包发版

```bash
python deploy/<PORT>/package-handheld.py
```

产出 `deploy/<PORT>-handheld.tar`，同时重写 `SHA256SUMS`（覆盖 `unityloader`、`unity.toml`、
`gamecontrollerdb.txt`、`unityloader.d/*.so`、`game/` 下全部文件），并打印包体大小与包哈希。
容器回放产物（seq 目录、png、ppm）不会被包进去——这是脚本设计目的，别手工往 tar 里塞东西。

桌面分发包用 `deploy/<PORT>/export-device-copy.py`：先校验掌机快照哈希，再解包、逐文件对 SHA256、再重新打包。
它开头硬编码了一个 `assert digest(...) == 'ff4ce4a8…'`，换快照必须同步改这个值，这是有意的防误操作。

---

## 2. 资产工具矩阵

| 工具 | 用途 |
|------|------|
| `inventory_bundle.py` | 只读：贴图 VRAM / 音频 / 类型占比 |
| `astc_retier.py` / `retier_all.sh` | 强制 ASTC；`--packer original` |
| `shrink_bundle.py --cap N` | 降长边；磁盘可能变大，看 RSS |
| `audio_stream_patch.py` | LoadType；内嵌包加 `--allow-embedded --mode hybrid` |

优先看**运行时 RSS**，不要只看磁盘体积。

**App Bundle 多 split 时**（Unity 6000.x 常见）：资产可能散在
`split_UnityDataAssetPack`、`split_UnityTextureCompressionsAssetPack` 等多个包里，
**且引擎必需件（`globalgamemanagers`、`level0`）不一定在最大的那个里**。
解包必须合并所有 split 的 assets，逐个 `unzip -l` 数文件，别按包名猜。参见 [`BLACKMOOR2.md`](BLACKMOOR2.md) §2。

---

## 3. 核心能力（勿再写进标题插件）

| 能力 | 配置 / API |
|------|------------|
| 进程内存 | `[debug] mem_log_interval_ms`；**可省略**（默认 2000 ms），`BD_MEM_LOG_MS` 环境变量优先，`0` 关闭；需 `BD_ENABLE_LOG`（关掉时整段编译移除，不再每 2 s 读 `/proc`）；标签 `[BD-MEM]` |
| 分辨率 / 刷新率自动探测 | `[device] displayWidth/Height/RefreshRate`；`0` 或省略 = 探测（SDL → `/dev/fb0` → 640x480@60）。实现 `platform/common/device_display.cpp`，日志标签 `[BD-DEVICE]` |
| PAD Java stub | `[play_asset_delivery] enabled/default_pack/pack_version/pack_path_template` |
| 每帧 present 钩子 | Plugin ABI v6：`register_present_callback` |
| 手柄捕获 / UI 光标 | ABI v8 可选尾部：`set_controller_capture` / `set_present_cursor`，需核对 `struct_size`；[Space Squids 示例](SPACESQUIDS_CONTROLLER.md) |
| 掌机震动 | 通用 `android.os.Vibrator` → SDL joystick rumble，需 `[input] controller = true`；[接口、限制与验证](HAPTICS.md)，Space Squids 已确认掌机输出有效 |
| 系统音量 | `[audio] backend/sysfs_path/sysfs_max/intercept_volume_keys`（`auto` 默认），`BD_SYS_VOLUME_*` 环境变量优先；sysfs 音量源 → 共享混音器统一施加增益，[机制、限制与验证](SYS_VOLUME.md) |
| 模块生命周期 | Plugin ABI v6：`find_module(name)` / `register_module_loaded(name, cb, userdata)` |
| CPU present / swap pause | `BD_EGL_CPU_PRESENT`、`BD_EGL_SWAP_PAUSE`（插件可 setenv） |

一份 TOML 适配多机型：`[device]` 的三项留 `0` 即可跟随面板；只有需要 letterbox / 逻辑分辨率与物理面板不同时才写死。JNI 桩（`Display`、`DisplayMode`、`ANativeWindow`、`eglQuerySurface`、纹理上传判定）全部读同一份探测结果，勿再各自查 toml。

配置项一律**可省略**：`[device]` 的 `displayWidth/Height/RefreshRate` 省略即探测，`[debug] mem_log_interval_ms` 省略即 2000 ms（整个 `[debug]` 表都可以不写）。给掌机铺配置时先只写必要项，别把默认值抄进去。

标题专属逻辑放 `unityloader.d/<port_id>.so`。

---

## 4. 插件约定

1. `abi_version == BOGODROID_PLUGIN_ABI_VERSION`（当前 **8**）。
2. 每帧工作用 `register_present_callback`，不要让核心 `dlsym` 游戏名符号。
3. 非 IL2CPP 模块 hook 先用 `find_module()` 查询；若模块稍后才加载，用 `register_module_loaded()`。
4. PAD 路径补丁 / IL2CPP hook 留在插件。

**边界**：插件里该有的是 IL2CPP hook、触摸注入与坐标换算、美术资源、启动视频时序之类**这款游戏专属**的东西。
不该有的是：任何"改一行 core 就能解决"的临时补丁，以及别的游戏的逻辑（**不要把一个游戏的插件部署到另一个游戏**）。

坐标换算原则：运行时取 RectTransform 实际中心（`Transform.TransformPoint`）再换算，**不写死像素坐标**——否则宽屏必偏。

---

## 5. 选游戏与引擎初判

| 信号 | 含义 |
|------|------|
| `libil2cpp.so` + `global-metadata.dat` | Unity IL2CPP → 本仓库主线 |
| `libyoyo.so` | GameMaker → **不是** unityloader 路径（另起运行时） |
| 大 Addressables / Play Asset Delivery 包 | 先做 inventory；标题峰值近 1GB 则止损 |

**加固 / 混淆的信号**：出现 `libpglarmor.so`、`libnms.so` 这类库，且 `addr2line` 反解 libil2cpp
得到的是 `DllCanUnloadNow` / `JNI_OnUnload` 之类明显不相干的符号 → 符号表被破坏，
**别指望反汇编定位崩溃**，改用日志 + 行为二分法。

**Unity 版本带来的已知差异**（开工前先对照，别在容器里一轮轮撞）：

| 差异点 | 2018.x | 6000.x | 详细 |
|---|---|---|---|
| 玩家类 | `UnityPlayer` 全包 | native* 搬到 `UnityPlayerForActivityOrService`，`initJni` 仍留基类 | [`UNITY6.md`](UNITY6.md) |
| `initJni` 签名 | 三参（带 cmdline） | 可能两参 `(Context,int)` | [`UNITY6.md`](UNITY6.md) |
| `Class.forName` | 单参够用 | 三参 `(String,boolean,ClassLoader)` 未 hook 会拿到 null jclass | [`UNITY6.md`](UNITY6.md) |
| 资产打包 | 单 APK / OBB | App Bundle 多 split，引擎必需件可能藏在最小那个包里 | §2、[`BLACKMOOR2.md`](BLACKMOOR2.md) §2 |
| 广告 / 内购 SDK | 视游戏而定 | 普遍带 UMP 同意流程，缺桩 → NRE → 线程池崩溃 | [`BLACKMOOR2.md`](BLACKMOOR2.md) §3.5 |

从第二个游戏起，先花 10 分钟读完这张表和 `UNITY6.md` 再动手。
这些坏点不预先知道，会在容器回放阶段**一次只暴露一个**，每轮重建 10 分钟，代价很高。

已完成的端口，代码即文档：`plugins/brickgamepro/`（完整样本）、
`plugins/oddmar_input/`、`oddmar_video/`、`oddmar_assetlocator/`（按键语义 / 视频 / 资产定位怎么拆插件）、
`plugins/hollow_knight_viewport/`（视口修正）、`plugins/skul_pad/`（手柄映射）、
`plugins/spacesquids/`、`plugins/samurai2_offline/`（其它形态的入口替换）。

---

## 6. 日志口径

**本文档只认这张表**，它与 `AGENTS.md`、`BUILD-DOCKER.md` 的写法保持一致；日后如有分歧，以本节为准。

| 场景 | 构建 | 产物 |
|---|---|---|
| 发版 / 交给用户 | Release + LOG/TRACE/VERBOSE/IL2CPP_TRACE **全 OFF** + `strip` | `build-device-release/unityloader`，约 5.7 MB |
| 排查 / 容器回放取证 | **RelWithDebInfo**（`-O2 -g`）+ `BD_ENABLE_LOG=ON` + 不 `strip`；要读 `viaGUI=` / `installed=` 一类行就够用 | `build-anbernic-rel/unityloader`，约 90–168 MB，只临时上机 |
| 深挖 | 在上一档基础上加 `BD_ENABLE_TRACE=ON` | 用完重编回发版档，别留在包里 |

判正式包对不对：跑起来的 `log.txt` 应该只剩 SDL/GL/FFmpeg 原生噪声，**零 `[BD-*]` 行**。
出现 `[BD-*]` 说明拿错二进制了——例外是崩溃时的 `[BD-SEGV]`（回溯不经 `BD_ENABLE_LOG`，见 §6.1）。

静态核对别用 `strings | grep '^\[BD-'` 判空：`[BD-SEGV]` 与 `[BD-J2]` 是**无条件编入**的字符串，发版包里永远有。
要判的是有没有 LOG 层标签（`[BD-MEM]` / `[BD-TIME]` / `[BD-INPUT]` …），命令见 [`../BUILD-DOCKER.md`](../BUILD-DOCKER.md) §4。

日志是**编译期**开关，改 `unity.toml` 不会多出任何一行；没有 TOML 分类过滤器，也没有 `[logging].exclude`。

### 6.1 标签速查

`BD_LOG`（LOG 档）——`BD_ENABLE_TRACE` 关着时这些是全部：

| 标签 | 源文件 | 出现时机 |
|---|---|---|
| `[BD-TIME]` | `projects/unityloader/main.cpp` | 启动各阶段耗时（`+N ms <stage>`） |
| `[BD-MEM]` | `loader/so_util.cpp` | 每个 .so mmap 的 base / size，一行 |
| `[BD-DATADIR]` | `thunks/libc/fcntl.cpp`、`javastubs/android_content.cpp` | Android files dir 解析结果 |
| `[BD-PREFS]` | `javastubs/android_content.cpp` | SharedPreferences load/save |
| `[BD-MKPARENT]` | `thunks/libc/stdio.cpp` | `open()` 自动建父目录 |
| `[BD-CAP]` | `thunks/khronos/gles2.cpp` | textureMaxDim 启用值，启动时一次 |
| `[BD-INPUT-REMAP]` | `platform/common/input_backend.cpp` | `unity.toml` 有 key remap 时 |
| `[BD-INPUT]` | `platform/common/input_backend.cpp`、`javastubs/android_view.cpp` | 控制器发现、轴校准、关键错误 |
| `[BD-JNI]` | `javastubs/javac.cpp`、`projects/unityloader/main.cpp` | `System.load / loadLibrary` |
| `[BD-SYM]` | `loader/so_util.cpp` | 链接没解析到的符号 |
| `[BD-EXIT]` | `projects/unityloader/main.cpp`、`platform/common/input_backend.cpp` | Unity 主动退出 / `Start+Select` 退出热键 |
| `[BD-OPENSLES]` | `thunks/opensles/opensles.cpp` | 默认编入（`BD_ENABLE_OPENSLES_SHIM=ON`） |

`BD_DEBUG`（TRACE 档）：`[BD-ASSET]` `[BD-WROPEN]` `[BD-RES]` `[BD-GRAPHICS-IO]` `[BD-STRGUARD]`
`[BD-DLOPEN]` `[BD-SYSCONF]` `[BD-PREFS-GET/PUT/EDIT/IO]`。

**不依赖任何开关的**：`fatal_error`（stderr 后 abort）、`[BD-SEGV]`（`platform/common/debug_utils.cpp`，`bd_crash_printf` 直打信号 / 地址 / 栈帧，不经 `BD_LOG`）。所以"LOG=OFF 就看不到崩溃"是错的。

`[JNIVM] [STUB-MISS] <kind>: Class=… Member=… Sig=… -> returning default`（`libjnivm/src/jnivm/internal/method.cpp`）是**关键诊断信号**：每个 (class+name+sig) 只打一次，反复刷屏才需要补桩或 `vm->setDefault<T>()`。

自查某个标签还在不在（行号与表会随代码漂）：

```bash
grep -rn 'BD_LOG("INPUT"' --include=*.cpp platform javastubs thunks projects
strings <build>/unityloader | grep -E '^\[BD-' | sort -u
```

### 6.2 已知无害噪音（不用排查）

| 行 | 来源 | 含义 |
|---|---|---|
| `xkbcommon: ERROR: couldn't find a Compose file for locale …` | libxkbcommon（SDL2 间接拉入） | 找不到本地化 Compose；不收文本输入，安全 |
| `arm_release_ver: … rk_so_ver: …` | Mali 厂商 GL 驱动 | 驱动启动时打的版本号 |
| `LOG[Unity]:` / `LOG[IL2CPP]:` | Unity / IL2CPP 引擎自身 | 不受 `BD_*` 开关控制 |

---

## 7. 复测清单

### 7.1 通用项（每个端口都要过）

- [ ] 构建命令带 `-DJNIVM_ENABLE_RETURN_NON_ZERO=OFF`，`CMakeCache.txt` 已核对
- [ ] `strip` 过，产物约 5–7 MB
- [ ] 容器回放：矩阵全部 case 通过，`verify-*.py` 无断言失败
- [ ] 640×480 与 1280×720 两种画面都回放过
- [ ] `package-handheld.py` 跑过，`SHA256SUMS` 已重写
- [ ] 真机推送后文件哈希对上，旧文件已备份
- [ ] 正式包 `log.txt` 零 `[BD-*]` 行
- [ ] 实例文档的发布摘要与 SHA256 表已更新

### 7.2 端口专属项

写在实例文档里，不写本文。BrickGame 的例子（不适用于别的游戏）：

- [ ] splash 至少 3 s → 开场动画 → 场景交接
- [ ] Guide 返回列表 / 列表 Guide 开退出框 / Start 暂停 / A 确认 / B 取消
- [ ] 电源键待机：熄屏、唤醒、音频与游戏更新都能恢复

---

## 8. 硬坑清单

| 现象 | 原因 | 处理 |
|---|---|---|
| 崩在不相干的 JNI 桩上，`exited 134` | `JNIVM_ENABLE_RETURN_NON_ZERO` 缓存残留 `ON` | 重编，显式 `=OFF`，核对 `CMakeCache.txt` |
| 真机黑屏，但容器正常 | 走了通用 GPU present viewport 的拷贝/裁切路径 | 保持插件驱动的 UGUI Canvas 路径，不启用 generic viewport |
| 改 UI 只动到背景，方块不跟着走 | 动态方块是世界空间对象，不是 UGUI `Grid` 的子节点 | 同时调 `Camera.main` 的 orthographic size 与位置 |
| 字体糊 | Canvas `scaleFactor=0.1875` + 2.2× 变换放大了低分辨率字形 | 按同一倍率调 `Text.pixelsPerUnit` |
| 触摸点偏 | 固定 640×480 坐标在宽屏下偏离按钮中心 | 运行时取 RectTransform 中心换算，别写死 |
| 退出后日志要关但排查要开 | 日志是编译期开关 | 两个构建目录分开：`build-device-release`（正式）/ `build-anbernic-rel`（排查） |
| Wwise 端口启动即 `Unknown symbol "slCreateEngine"` / `SIGABRT` | 构建时 `BD_ENABLE_OPENSLES_SHIM=OFF`（旧脚本或共享 cache 残留） | 显式 `=ON` 重编；该选项默认已是 ON，别手动关 |
| 编出来的还是旧代码 | `GlES_Dev` 无 bind mount | `docker cp` 进去，必要时 `touch` |
| 引擎报 `Unable to initialize the Unity Engine` | App Bundle 里引擎必需件在别的 split | 合并所有 split 的 assets，别按包名猜（[`BLACKMOOR2.md`](BLACKMOOR2.md) §2） |
| `initJni not found` | Unity 6000.x 类搬家 / 签名变化 | 见 [`UNITY6.md`](UNITY6.md) |
| 事件注入了 `result=1` 但游戏不响应 | 设备枚举、签名擦除、KeyEvent 翻译层 三选一 | 先给 `KeyEvent` getter 加日志看 Unity 读了什么 |
| `xdotool key` 看起来发了但无效 | 真实 X11 键盘事件不进 SDL 注入路径 | 这个测法不成立，用 `BD_PAD_REPLAY` |
| 真机打印完 `OpenGL ES 3.2 / Mali` 就退出 | Unity 拿 Android `display` 句柄调真 EGL，Mali 上踩 `eglQueryDevicesEXT` | `BD_FAKE_EGL=ON`（默认）走假实现且不转发，`getProc` 不 `dlopen("libEGL.so")` |

---

## 9. 文档维护契约

本文的价值全在"照着跑就能出包"。**任何一步失真的那一刻，它就变成了负债**——下个会话会照着过期结论做错事。
下表是强制的：左边改了，右边必须同步，同一次提交里改完。

| 你改了什么 | 必须同步改哪一节 | 为什么 |
|---|---|---|
| 构建命令本身（目录名 / flag / 是否 `strip`） | §0.3、§1.1、§1.5、§6 表 | 四处贴的是同一组命令，改一处不改其余就会互相打脸 |
| `scripts/container-run-game-seq.sh` 的环境变量 | §0.3、§1.6.2 参数表 | 回放参数是本文被抄最多的地方 |
| `scripts/verify-*.py` 的断言或用法 | §0.3、§1.6.4 | 断言变了，"过没过"的判据就变了 |
| 部署/推送流程、掌机路径 | §0.4、§1.7 | 会指向不存在的位置 |
| 日志开关口径 | §1.1、§6 | 历史上已经冲突过一次 |
| 插件边界、加载顺序 | §4 | 决定新功能该写在哪 |
| 踩了新坑并解决 | §8 硬坑清单 | 不写下来，下个会话会重踩 |
| **完成了某一环节的验证** | **§0.4 基线状态** | 这是新会话判断"能不能信"的唯一依据 |
| 新接一款游戏 / 新开端口 | 新建 `docs/<端口名>.md`，并在 `AGENTS.md` 入口表挂链接 | 过程留在实例文档，本文不被污染 |
| 发现本文任一处与现实不符 | 就地改本文 | memory 只记过程流水，**不承担规范职责** |

两条自我约束：

1. **不复制、只引用。** §0.3 的命令若在 §1 里也出现，§0.3 是索引、详版在正文章节。
2. **状态只写能验证的东西。** 禁止写"基本可用""大致跑通"。要么给证据路径，要么写 ❌。

### 9.1 为什么入口必须落在仓库内

规范入口只能挂在**任何人 clone 下来都会看到**的文件上：

| 层 | 文件 | 谁看 |
|---|---|---|
| 第 1 层 | `AGENTS.md` 顶部处境表 | 这是**唯一入口**。各类编码 agent（Claude Code / Cursor / Codex 等）会主动读 |
| 第 2 层 | 本文 + `docs/<端口名>.md` | 真要动手的人，从 AGENTS.md 点进来 |

**不要往 `README.md` 加端口相关引导。** README 是上游项目的说明（Bogodroid 是什么、
怎么编译上游代码），本仓库的移植流程属于下游玩法，混进去会让上游读者困惑。
入口一处即可，`AGENTS.md` 顶部处境表已经覆盖"新会话要知道从哪开始"这个需求。

`.workbuddy/memory/` 是**工具私有目录**，负责逐日流水，不负责传递规范。
把入口建在它上面，换个环境、换个人、或者工具换了，链路就断。

---

## 10. 相关文档

- [`UNITY6.md`](UNITY6.md) — 换 Unity 6000.x 的额外改动（类搬家、`initJni` 签名、`Class.forName`）
- [`BRICKGAMEPRO.md`](BRICKGAMEPRO.md) — BrickGame 实例：发布摘要、布局/相机参数、当前哈希
- [`BLACKMOOR2.md`](BLACKMOOR2.md) — Blackmoor 2 实例：App Bundle 解包坑、4 处通用 loader 缺陷、手柄卡点
- [`BRICKGAME_TODO.md`](BRICKGAME_TODO.md) — BrickGame 未完成项
- [`HAPTICS.md`](HAPTICS.md) — 震动接口与验证
- [`SYS_VOLUME.md`](SYS_VOLUME.md) — sysfs 音量源与软件增益（Anbernic 类固件）
- [`SPACESQUIDS_CONTROLLER.md`](SPACESQUIDS_CONTROLLER.md) — 手柄捕获 / UI 光标示例
- 根目录 [`AGENTS.md`](../AGENTS.md) — Dropbeak 大文件推送、日志编译期开关
- [`BUILD-DOCKER.md`](../BUILD-DOCKER.md) — Docker 交叉编译环境（镜像、CMake 选项表、产物检查）
