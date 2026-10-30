# 容器构建（GlES_Dev）

> © 2025-2026 jenny92-tech &lt;jennyliu90223@gmail.com&gt; · GPL v3
>
> **本页讲这个仓库实际怎么编出 `unityloader`**：在 `GlES_Dev` 容器里编。
> 流水线怎么走、装到掌机哪里、日志开不开——看 [`docs/PORTING_PLAYBOOK.md`](docs/PORTING_PLAYBOOK.md) 与 [`AGENTS.md`](AGENTS.md)。
> 上游那套 x86 Debian 交叉（`Dockerfile.builder` / `scripts/build-docker.sh`）已不可用，原因见 §5。

---

## 0. 链路与基线

```
宿主仓库  --docker cp-->  GlES_Dev:/workspace/Bogodroid  --cmake/ninja-->  unityloader
                                                                  --strip--> dropbeak push 掌机
```

容器基线（`docker exec GlES_Dev …` 实测）：

| 项 | 值 |
|---|---|
| 镜像 / 容器 | `dropbeak-gles-dev:local` → 容器名 `GlES_Dev` |
| 系统 | Ubuntu 20.04.6 LTS（glibc 2.31） |
| 编译器 | `aarch64-linux-gnu-g++` 9.4.0（`crossbuild-essential-arm64`） |
| CMake / 生成器 | 3.16.3 / Ninja |
| FFmpeg | 4.2 dev，`/usr/include/aarch64-linux-gnu/libavcodec/avcodec.h` |
| 源码副本 | `/workspace/Bogodroid`（**注意不是仓库名**） |

产物要求掌机 glibc ≥ 2.31：TrimUI Smart Pro 2.33、Anbernic H700 均满足。`-static-libstdc++ -static-libgcc` 已默认开启，防旧固件缺 `GLIBCXX_3.4.31`。

`/workspace/Bogodroid` 是**副本，不是 bind mount**。里面的 `.git` 是个失效的 worktree 指针（指向 `D:/Locke/gitee/Bogodroid/.git/...`），所以容器里 `git` 命令不可用，也不要指望它跟着宿主自动更新。

---

## 1. 同步源码

只推改动过的文件，别整树重推（副本已有 6.2 GB）：

```bash
# 目录级（项目/插件）
docker cp projects/unityloader/. GlES_Dev:/workspace/Bogodroid/projects/unityloader/
# 单文件
docker cp thunks/egl_sdl/egl_sdl.cpp GlES_Dev:/workspace/Bogodroid/thunks/egl_sdl/egl_sdl.cpp
```

- Git Bash 下 `docker cp` 用**相对路径**最稳；绝对路径必须是 `D:/…` 形式，`/d/…` 会被 MSYS 改写。
- `docker cp` 保留 mtime。增量构建靠 mtime 判断，若本机文件更旧但内容不同，改完 `touch` 一下再推。
- 同步完先自检：容器里 `ls -l <file>` 的时间戳与宿主一致，再动 cmake。

---

## 2. 构建

### 2.1 发版（LOG 全 OFF + strip，约 5.7 MB）

```bash
docker exec GlES_Dev bash -lc '
  cd /workspace/Bogodroid &&
  cmake -S . -B build-device-release -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBD_ENABLE_LOG=OFF -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF \
    -DIL2CPP_TRACE=OFF -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF &&
  cmake --build build-device-release -j"$(nproc)" \
    --target unityloader plugin_<port_id> &&
  aarch64-linux-gnu-strip build-device-release/unityloader \
    build-device-release/unityloader.d/<port_id>.so
'
```

- 插件有两个等价 target 名：`plugin_<port_id>`（`plugins/CMakeLists.txt:8`，`add_custom_target` 包装）和 `bogodroid_unityloader_plugin_<port_id>`（`plugins/CMakeLists.txt:6`，真正的库 target）。`--target` 用哪个都行。**产物文件名是 `<port_id>.so`**（`OUTPUT_NAME "${name}"` + `PREFIX ""`），落在 `unityloader.d/`。
- 没有插件就只编 `--target unityloader`。
- `-DCMAKE_TOOLCHAIN_FILE` 保留是历史一致：容器本身就是 aarch64，等于同架构"交叉"，产物与原生一致。删掉也能编，但会让缓存与既有目录不一致。

### 2.2 排查（LOG=ON，不 strip）

用 `RelWithDebInfo`（`-O2 -g`，保留符号与日志，跑得动），换一个目录，不 strip：

```bash
docker exec GlES_Dev bash -lc 'cd /workspace/Bogodroid &&
  cmake -S . -B build-anbernic-rel -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBD_ENABLE_LOG=ON -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF \
    -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF &&
  cmake --build build-anbernic-rel -j"$(nproc)" --target unityloader'
```

要 `BD_DEBUG` 级细节再自己加 `-DBD_ENABLE_TRACE=ON`。**日志档不要发出去**：产物约 90–168 MB，且 `log.txt` 会刷满 `[BD-*]`。

`BD_ENABLE_OPENSLES_SHIM` 别写 `OFF`——默认已是 ON，写成 OFF 在 Wwise 端口上启动即 `SIGABRT`（`docs/PORTING_PLAYBOOK.md` §8）。

### 2.3 增量与缓存

- 同目录只跑 `cmake --build <dir> -j$(nproc)`：改一个文件约 45–50 s。
- **`cmake -S . -B` 会改写 `CMakeCache.txt`**：`build-device-release` 上一次被谁用过，cache 里就是谁的口径（实测现在 `BD_ENABLE_LOG:BOOL=ON`）。所以发版和排查**用两个目录**（`build-device-release` / `build-anbernic-rel`），别共用一个。
- 宿主上那几个 `build-trimui-*`、`build-spacesquids-container` 是从旧容器里拿出来的，cache 里的绝对路径是 `/workspace/Bogodroid-brickgame/…` 或 `/work/…`，在现在的 `/workspace/Bogodroid` 下**无效**。要用就重配，别指望拷进去直接 `ninja`。

---

## 3. CMake 选项

以 `CMakeLists.txt` 的 `option()` 为准（行号会漂）：

```bash
grep -nE '^\s*option\(' CMakeLists.txt
```

| 选项 | 默认 | 作用 |
|---|---|---|
| `BD_ENABLE_LOG` | OFF | **日志主开关**：`BD_LOG` / `[BD-MEM]` / 插件 `api->log` / jnivm `LOG` |
| `BD_ENABLE_TRACE` | OFF | 需 LOG：`BD_DEBUG` / `warning` / `BOOT_LOG` |
| `BD_ENABLE_VERBOSE` | OFF | 需 LOG：`verbose()`，行数极多 |
| `IL2CPP_TRACE` | OFF | 需 LOG：il2cpp 内部 trace |
| `BD_ENABLE_OPENSLES_SHIM` | **ON** | OpenSL ES → SDL 桥，按调用方分发（只给 Wwise，FMOD 走 fakefmod） |
| `BD_FAKE_EGL` | **ON** | SDL 建好 GLES 上下文后，Unity 的 EGL 调用不再转给系统 libEGL（Anbernic / Mali） |
| `BD_ENABLE_GPLAY` | OFF | Google Play Games 桩；不开省约 340 KB |
| `BD_ENABLE_MOBGE_ASSETLOCATOR` | OFF | Oddmar 专用 `com.mobge.assetlocator` 桩；首选 `oddmar_assetlocator` 插件 |
| `USE_MOLD` | OFF | mold 链接器；交叉工具链文件里强制 OFF |
| `USE_LLD` | OFF | lld 链接器 |

`BD_ENABLE_TRACE / VERBOSE / IL2CPP_TRACE` 开了但 `BD_ENABLE_LOG=OFF` 会 `FATAL_ERROR`。

`Release`（含 `CMAKE_BUILD_TYPE` 未定义）额外启用 `-ffunction-sections -fdata-sections -fvisibility=hidden -flto` 与 `-Wl,--gc-sections -flto -s`。**LTO 在 ninja 下是串行的**，全量链接可能 10 min 起；只想快速迭代就 `RelWithDebInfo`（不带 LTO）。

FFmpeg 运行时不进 `DT_NEEDED`：`thunks/ndk/media.cpp` 用 `dlopen` 按需加载。编译期仍然要头文件——这也是为什么构建必须在带 FFmpeg dev 的容器里做（§5）。

---

## 4. 产物检查

```bash
file build-device-release/unityloader                    # ELF 64-bit, ARM aarch64, stripped
objdump -p build-device-release/unityloader | grep NEEDED
LC_ALL=C grep -ao 'GLIBC_[0-9.]*' build-device-release/unityloader | sort -uV | tail -3
strings build-device-release/unityloader | grep -E '^\[BD-' | sort -u
```

当前发版产物的正常长相（`build-trimui-release/unityloader`，5,705,976 字节）：

```
NEEDED: libdl.so.2  libc.so.6  ld-linux-aarch64.so.1  libSDL2-2.0.so.0  libpthread.so.0  libz.so.1  libm.so.6
最高 GLIBC 符号: GLIBC_2.30
strings '^[BD-': [BD-SEGV] signal … / [BD-J2] native=… （这两条恒在，不算日志开关）
```

判读：

- `file` 说 `x86-64` → 用错了容器或漏了交叉工具链，重新配。
- `NEEDED` 出现 `libav*` / `libEGL*` → 链接口径不对。`libSDL2-2.0.so.0`、`libz.so.1` 由掌机系统提供，属正常。
- `GLIBC_2.xx` 高于掌机版本 → 换了更新的基础镜像，退回 Ubuntu 20.04 基线。
- `strings` 出现 `[BD-MEM]` / `[BD-TIME]` / `[BD-INPUT]` 这类 → 这是**带 LOG** 的包，发版不合格，见 playbook §6。`[BD-SEGV]`（崩溃回溯）与 `[BD-J2]`（运行时 `BD_J2_TRACE` 才触发）无条件编入，不能当判据。

---

## 5. 已废弃：x86 Debian 交叉（`Dockerfile.builder` / `build-docker.sh`）

上游时代留下的第二条路：x86 主机上用 `bogo-builder`（Debian 11）交叉编 aarch64。**现在跑不通，别再试**，理由按发现顺序：

1. 镜像里**没有 FFmpeg dev**，而 `thunks/ndk/media.cpp`、`thunks/ndk/ffmpeg_optional.cpp`、`projects/unityloader/startup_video.cpp` 无条件 `#include <libavcodec/avcodec.h>` —— 编译直接 `fatal error: libavcodec/avcodec.h: No such file or directory`（2026-10-29 实测复现）。CMake 那边只有 `pkg_check_modules(FFMPEG QUIET …)`，找不到也只打一行 STATUS，不报错。
2. 想补装也不行：Debian 11 已 EOL，`deb.debian.org/debian-security` 上 ffmpeg 包 404；换 `archive.debian.org` 会因索引版本倒退被 apt 为了解依赖**卸掉 `cmake` / `libsdl2-dev`**（2026-09-21 记录，`apt-get install -s` 可预演）。
3. 顺带发现的两个历史镜像也都残：`bogo-builder:oddmar-ffmpeg` 有 FFmpeg 但**没 cmake**；`bogo-builder:unity2017-armv7` 有 cmake 但**没 FFmpeg 头**。

所以 `scripts/build-docker.sh` 与 `scripts/unity6/build-rel.sh` / `build-log.sh` 属于没落地的路线：它们要求的 `build-unity6/` 从未成功配出来。留着当历史参考，日常别用。

要恢复这条路，前提是换掉 `FROM debian:11`、把 FFmpeg dev 补进配方、并重新验证 glibc 基线——那是独立的一件事，不是改两行 flag。

---

## 6. 常见坑

| 现象 | 原因 | 解法 |
|---|---|---|
| 改了代码，容器里编出来没变 | 只改了宿主，没 `docker cp` 进副本 | 同步后核对容器内文件 mtime |
| `fatal: not a git repository: …/worktrees/…` | 副本里的 `.git` 是失效 worktree 指针 | 容器里不做 git 操作，版本以宿主为准 |
| `ninja: unknown target bogodroid_unityloader_plugin_x` | 那是产物名不是 target | 用 `plugin_x` |
| 链接阶段卡 10 分钟以上 | Release 带 LTO，ninja 下 ltrans 串行 | 迭代期用 `RelWithDebInfo`，或等 |
| 发版包跑起来刷 `[BD-*]` | 用了 `build-device-release` 里上次留下的 `BD_ENABLE_LOG=ON` | 发版单独目录；显式传 `-DBD_ENABLE_LOG=OFF` 并核对 cache |
| 拷回宿主的 `build-trimui-*` 在容器里 `ninja` 报路径错 | cache 里是旧绝对路径 `/workspace/Bogodroid-brickgame` | 删掉重配，别跨环境搬构建目录 |
| `docker cp` 报 `Cannot connect to D:` | Git Bash 把 `D:/…` 当远程主机 / tar 行为差异 | 用相对路径；`tar` 加 `--force-local` |
| Debug 构建测帧率偏低 | `-O0` | 用 `RelWithDebInfo` 复测 |

---

## 7. 引用

- 容器内构建命令（发版 / 排查两档）：[`docs/PORTING_PLAYBOOK.md`](docs/PORTING_PLAYBOOK.md) §1.1、§1.5
- 交叉工具链：[`cmake/aarch64-linux-gnu.cmake`](cmake/aarch64-linux-gnu.cmake)
- 历史配方（不可用）：[`Dockerfile.builder`](Dockerfile.builder)、[`scripts/build-docker.sh`](scripts/build-docker.sh)
- 日志宏单一来源：[`platform/common/logging.h`](platform/common/logging.h)
- 完整流程：[`docs/PORTING_PLAYBOOK.md`](docs/PORTING_PLAYBOOK.md)
