# BogoDroid NEO — Docker 交叉编译流程

> © 2025-2026 jenny92-tech &lt;jennyliu90223@gmail.com&gt; · GPL v3

> **此文档为 AI 阅读用。**未来的 Claude 会话读完本文档后应当能直接复现：
> 在 macOS / x86 Linux 主机上用 Docker 交叉编译出 ARM64 ELF
> （`unityloader`），并部署到 R36S Pro 等 ARM64 PortMaster 掌机。
>
> 上游 README 里的 `How to Build` 是 Linux 原生构建路径，**不适用**于本工作流。

---

## 0. 变量约定

所有命令引用以下变量。AI 在执行前应当从 cwd / 用户提示中解析出实际值，不要写死路径：

| 变量 | 含义 | 解析方式 |
|---|---|---|
| `REPO` | 本仓库根（含 `CMakeLists.txt` / `Dockerfile.builder`）| `pwd`，或 cwd 的根 |
| `SD_PORT` | SD 卡上 PortMaster 端口目录（含 `wsm.toml` / `unityloader` / `lib/arm64-v8a/`） | 必须从用户或环境探测（macOS 通常挂在 `/Volumes/<label>/roms/ports/<port>/`），AI 不要硬编码 |

下文写 `$REPO` / `$SD_PORT` 时一律指上面这两个变量。

---

## 1. 目录结构

仓库根 (`$REPO`)：

```
$REPO/
├── Dockerfile.builder       预装工具链的镜像配方
├── BUILD-DOCKER.md          本文档
├── tomlplusplus/            submodule，首次 clone 后必须 init
├── CMakeLists.txt           构建入口；feature flag 在 207–255 行
├── platform/common/logging.h  日志宏单一来源 (BD_LOG / BD_DEBUG / fatal_error)
└── build-release/           Release 构建输出 (我们的产物)
    └── unityloader          ← 目标 ELF
```

SD 卡端口目录 (`$SD_PORT`)：

```
$SD_PORT/
├── unityloader              build-release/unityloader 拷贝过来
├── wsm.toml                 运行时配置 (paths / unity / input / gpu / debug 段)
├── lib/arm64-v8a/           游戏自带的 *.so (libil2cpp / libmain / libc++_shared / ...)
├── gamedata/                APK 解包出来的 assets / global-metadata.dat 等
└── log.txt                  运行时 stderr 重定向到这里
```

---

## 2. 一次性准备

### 2.1 拉子模块

```bash
cd "$REPO"
git submodule update --init --recursive
```

### 2.2 建 builder 镜像

镜像里预装了 cmake / ninja / mold / g++ / SDL2 / GLES / libbsd。**只需建一次**：

```bash
docker build --progress=plain --platform linux/arm64 \
  -t bogo-builder \
  -f "$REPO/Dockerfile.builder" "$REPO"
```

首次约 1–2 分钟（拉 ubuntu:22.04 + 装 ~150 MB dev 包）。之后镜像在本地，下次跳过。

判断是否已存在：

```bash
docker images --format '{{.Repository}}' | grep -q '^bogo-builder$'
```

---

## 3. 日常：编 unityloader

默认 **Release** 配置（4.9 MB 产物，LTO + strip），输出在 `$REPO/build-release/`。

**增量构建**（改了几个 .cpp）：

```bash
docker run --rm --platform linux/arm64 \
  -v "$REPO:/work" -w /work/build-release \
  bogo-builder ninja unityloader
```

**全量重链接**（换分支 / 改链接 flag 时）：

```bash
docker run --rm --platform linux/arm64 \
  -v "$REPO:/work" -w /work/build-release \
  bogo-builder bash -c "ninja -t clean && ninja unityloader"
```

**首次初始化 / `CMakeCache.txt` 损坏 / 换 build type**，重跑 cmake：

```bash
docker run --rm --platform linux/arm64 \
  -v "$REPO:/work" -w /work/build-release \
  bogo-builder bash -c "rm -rf * && cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja unityloader"
```

---

## 4. CMake 可选 flag

通过 `cmake -DXXX=ON ..` 传入。源代码定义在 `CMakeLists.txt` 207–255 行。

| Flag | 默认 | 作用 |
|---|---|---|
| `CMAKE_BUILD_TYPE=Release` | Debug | 启用 LTO + `-fvisibility=hidden` + `-Wl,--gc-sections -s`。9.5 MB → 4.9 MB |
| `BD_ENABLE_LOG=ON` | OFF | **日志主开关**。打开 `BD_LOG` + jnivm `LOG` 事件行。所有 sub-trace 开关都依赖它，否则 cmake `FATAL_ERROR` |
| `BD_ENABLE_TRACE=ON` | OFF | 在 LOG 之上额外打开 `BD_DEBUG` / `warning` / `BOOT_LOG` 细节诊断行。需 `BD_ENABLE_LOG=ON` |
| `BD_ENABLE_VERBOSE=ON` | OFF | 在 LOG 之上额外打开 `verbose()`（365 处 legacy）。需 `BD_ENABLE_LOG=ON` |
| `IL2CPP_TRACE=ON` | OFF | 在 LOG 之上额外打开 il2cpp 内部 trace（仅 unityloader）。需 `BD_ENABLE_LOG=ON` |
| `BD_ENABLE_OPENSLES_SHIM=ON` | OFF | 把 `thunks/opensles/` 编进 + 暴露给 FMOD。当前 AudioTrack/fakefmod 路径已经能用，此 flag 保留为后续 A/B |

---

## 5. 部署到 SD 卡

```bash
# 1. 确认 SD 卡已挂载且端口目录存在
[ -d "$SD_PORT" ] || { echo "ERR: $SD_PORT 不存在，请确认 SD 已挂载"; exit 1; }

# 2. 拷贝 + 强制刷写
cp "$REPO/build-release/unityloader" "$SD_PORT/unityloader"
sync
```

**必须 `sync`**：macOS 写缓存默认是异步的，不 sync 拔卡会回滚。

---

## 6. 健全性检查

```bash
ls -la "$SD_PORT/unityloader"                                          # 期望 ~4.9 MB
strings "$SD_PORT/unityloader" | grep -E '^\[BD-' | sort -u            # BD-* 字符串清单
```

### 6.1 Release 二进制里应该出现的 BD-* 标签

`BD_LOG` 是事件级日志，默认 **关闭**；编译时加 `-DBD_ENABLE_LOG=ON` 才会出现以下标签：

| 标签 | 源文件 | 出现时机 |
|---|---|---|
| `[BD-TIME]` | `projects/unityloader/main.cpp` | 启动各阶段耗时（"+N ms <stage>"）|
| `[BD-MEM]` | `loader/so_util.cpp` | 每个 .so mmap 的 base / size，一行 |
| `[BD-DATADIR]` | `thunks/libc/fcntl.cpp` | Android files dir 解析结果 |
| `[BD-PREFS]` | `javastubs/android_content.cpp` | SharedPreferences load/save 关键事件 |
| `[BD-MKPARENT]` | `thunks/libc/stdio.cpp` | open() 自动建父目录时 |
| `[BD-CAP]` | `thunks/khronos/gles2.cpp` | textureMaxDim 启用值，启动时一次 |
| `[BD-INPUT-REMAP]` | `platform/common/input_backend.cpp` | wsm.toml 有 key remap 时 |
| `[BD-INPUT]` | `platform/common/input_backend.cpp` | SDL_Init 失败等关键错误 |
| `[BD-JNI]` | `javastubs/javac.cpp` | `System.load / loadLibrary` 调用 |
| `[BD-SEGV]` | `platform/common/debug_utils.cpp` | SIGSEGV 处理器 |
| `[BD-SYM]` | `loader/so_util.cpp` | 链接没解析到的符号 |
| `[BD-EXIT]` | `projects/unityloader/main.cpp` | Unity 主动退出 (nativeRender 返回 false) |
| `[BD-OPENSLES]` | `thunks/opensles/opensles.cpp` | 仅 `-DBD_ENABLE_OPENSLES_SHIM=ON` 编进时 |

默认 build（不传 `-DBD_ENABLE_LOG=ON`）**以上全没**。崩溃信息仍然会打：`fatal_error` 走 stderr，`SIGSEGV` 走 libc `backtrace_symbols_fd` 直接打栈帧符号（不经 BD_LOG）。

### 6.2 仅在 debug / `-DBD_ENABLE_TRACE=ON` 出现的 BD-* 标签

`BD_DEBUG` 是 NDEBUG-gated，Release 编译成 `((void)0)`：

`[BD-ASSET]` `[BD-WROPEN]` `[BD-RES]` `[BD-PREFS-GET]` `[BD-PREFS-PUT]`
`[BD-PREFS-EDIT]` `[BD-PREFS-IO]` `[BD-GRAPHICS-IO]` `[BD-STRGUARD]`
`[BD-DLOPEN]` `[BD-SYSCONF]`

要在 release 里临时启用，重编：`cmake .. -DCMAKE_BUILD_TYPE=Release -DBD_ENABLE_TRACE=ON`。

### 6.3 log.txt 里的已知噪音（无害，不用排查）

| 行 | 来源 | 含义 |
|---|---|---|
| `xkbcommon: ERROR: couldn't find a Compose file for locale "en_US.UTF-8"` | libxkbcommon (被 SDL2 间接拉进来) | 找不到本地化 Compose；不收文本输入，安全 |
| `arm_release_ver: g13p0-..., rk_so_ver: 10` | Mali-G52 GPU 驱动 | RK3566 厂商 GL driver 启动时打的版本号 |
| `` [JNIVM]: [STUB-MISS] <kind>: Class=`...` Member=`...` Sig=`...` -> returning default `` | `libjnivm/.../method.cpp` | 游戏调了没实现的 Java 成员。**关键诊断信号**：每个 (class+name+sig) 只打 1 次（dedup）。反复刷屏才需要补 stub 或加 `vm->setDefault<T>()` |
| `LOG[Unity]:` / `LOG[IL2CPP]:` | Unity / IL2CPP 引擎自己 | 控不了 |

---

## 7. 常见坑

| 现象 | 原因 | 解法 |
|------|------|------|
| `ninja: command not found` | 没用 `bogo-builder`，直接跑了 ubuntu 镜像 | 用 `bogo-builder` |
| `mold not found` / 链接失败 | 镜像版本旧 | 重建 builder 镜像 |
| 警告 `using serial compilation of N LTRANS jobs` | LTO 串行，正常 | 忽略 |
| 二进制 ~9 MB | Release flags 没启用 | 看 `build-release/CMakeCache.txt`：`CMAKE_BUILD_TYPE:STRING=Release` |
| segfault on dlopen | `$SD_PORT/lib/arm64-v8a/*.so` 缺失或权限不对 | `chmod 755 $SD_PORT/lib/arm64-v8a/*.so`，确认游戏自带的 4–6 个 so 都在 |
| `Couldn't open Display` | EGL 初始化失败，多半是 SDL2 与 Mali 驱动不兼容 | 看 log.txt 里 `OpenGL Version` 那行 |
| log.txt 末尾被截断 | 应已修复（main.cpp 里 `setvbuf(stderr, _IONBF, 0)`）；仍出现则 build 太老 | 重新 build + 部署 |

---

## 8. 一键脚本模板

把以下保存为 `~/bin/bogo-deploy.sh`，开头两个变量按本机情况改：

```bash
#!/usr/bin/env bash
set -e
: "${REPO:?REPO must be set (the Bogodroid checkout)}"
: "${SD_PORT:?SD_PORT must be set (e.g. /Volumes/<label>/roms/ports/<port>)}"

echo "[1/3] building..."
docker run --rm --platform linux/arm64 \
  -v "$REPO:/work" -w /work/build-release \
  bogo-builder ninja unityloader

echo "[2/3] deploying..."
[ -d "$SD_PORT" ] || { echo "ERR: SD card not mounted at $SD_PORT"; exit 1; }
cp "$REPO/build-release/unityloader" "$SD_PORT/unityloader"
sync

echo "[3/3] verifying..."
ls -la "$SD_PORT/unityloader"
echo "BD-* tags embedded:"
strings "$SD_PORT/unityloader" | grep -E '^\[BD-' | sort -u | sed 's/^/  /'
echo "done."
```

调用：`REPO=... SD_PORT=... bogo-deploy.sh`

---

## 9. 引用

- 镜像配方：[`Dockerfile.builder`](./Dockerfile.builder)
- 上游构建文档：[`README.md`](./README.md) §How to Build（Linux 原生路线，仅参考）
- CMake flag 块：[`CMakeLists.txt`](./CMakeLists.txt) 207–255 行
- 日志宏定义：[`platform/common/logging.h`](./platform/common/logging.h)
