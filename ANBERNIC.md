# Anbernic：unityloader 构建、部署与已验证状态

> 日期：2026-08-31 起稿；**2026-09-01 空洞骑士已能玩**  
> 工作目录：本仓库 `D:\Locke\gitee\Bogodroid`  
> 目标：在 Anbernic Linux ARM64 掌机（Ports 菜单 / `dmenu.bin`，**不是 Weston**）上，用 `unityloader` 加载 Unity 2021/2022 安卓包（IL2CPP + ARM64）  
> 通道：Dropbeak（`D:\Locke\gitee\dropbeak`）推二进制、拉 `log.txt`  
> 构建容器：**只在 `GlES_Dev`**。不要在 `nostalgic_faraday` 里编 unityloader。

本文是改代码、编 ELF、真机验证的单一入口。Dropbeak 侧调查纪要见 `D:\Locke\gitee\dropbeak\BOGODROID.md`。

---

## 现状（2026-09-05 已验证）

空洞骑士（Unity 2020.2.2f1 IL2CPP ARM64）从 Ports 菜单点 **`hk`** 可进关、可操作、系统音量与游戏内音量键有效。

同一个最新版 Release/LTO `unityloader` 已在 Samurai II、Unity 2021、
Unity 2022 和 Hollow Knight 四个 Ports 目录中运行通过并正常退出。

| 项 | 结果 |
|---|---|
| GLES | 3.2 / Mali-G31；假 EGL（`BD_FAKE_EGL=ON`）；`SDL_GL_SetSwapInterval(0)` |
| 分辨率 | 物理 640×480；插件把 16:9 相机铺满 4:3 |
| 按键 | 内置 `ANBERNIC-keys` mapping；`dpad_synthesize_hat=false` |
| 音频 | FMOD OpenSL 失败 → AudioTrack/`fakefmod` → SDL → ALSA。**不要**开 `BD_ENABLE_OPENSLES_SHIM` |
| 音量 | `openbor_volume` sysfs（0–10）→ fakefmod 软件增益；游戏内 `VOLUMEUP`/`DOWN` 写回 sysfs |

测试入口（独立于旧 PortMaster 项 `K_空洞骑士[中].sh`）：

```text
/mnt/mmc/Roms/ports/hk.sh
/mnt/mmc/Roms/ports/hk/
  unityloader, hk.toml, gamecontrollerdb.txt,
  gamedata/, conf/, cache/,
  unityloader.d/hollow_knight_viewport.so
```

本地对照：`D:\Locke\gitee\dropbeak\examples\deploy\` 下 `hk.sh`、`hk.toml`、`hk-boot.config`、`hk-GraphicsSettings.txt`、`hk-playerprefs.json`。  
卡分区是 **VFAT**，脚本必须 LF；不要 symlink。

下文 §3 起原先按「计划」写的条目，凡标「尚未改」的假 EGL 路径、`fatal_error`、系统 SDL，**已经落地**。仍有效的是构建/部署约束（§1、§4、§5、§7）。

---

## 0. 先说清楚：本目录现在是什么

| 树 | 路径 / 远程 | 实质 |
|---|---|---|
| **本仓库（当前 HEAD）** | `end1220/Bogodroid` · 分支 `main` · `5b07c10` | NEO **之前**的实验树。ELF32、`armeabi-v7a`、写死 Mono `libmonobdwgc-2.0.so`。没有 `thunks/egl_sdl/`。README 写明 Unity 初始化约 70% 后，Mono 建第一条线程即崩。 |
| **NEO 基线（应作为改造起点）** | `C:\Users\Administrator\Desktop\Jump2021\vendor\Bogodroid` · `jenny92-tech/Bogodroid` · 分支 **`2021.3`** · GPL v3 | ELF64 + IL2CPP + `thunks/egl_sdl`。已能在部分掌机上跑简单 Unity 2021 和空洞骑士。桌面 `Jump2021\portmaster\` 用同一套 loader 跑过 **Unity 2021.3.45f2** IL2CPP ARM64。 |

**不要**在本仓库的 ELF32 代码上「升级到 2022」——那是重复 NEO 已经做完的 ELF64 / RELA / IL2CPP / EGL-SDL。

**要做的是：** 把 NEO `2021.3` 整树合进本目录（保留本仓库 `.git` 或改 remote），然后在本目录针对 Anbernic 继续改。改过的 `unityloader` 若对外分发，须继续 GPL v3 并保留 jenny92-tech 署名。游戏资源不得进本仓库。

空洞骑士那份 `unityloader` **不要反编译**：源码就是 NEO。`plugins/hollow_knight_viewport/`、HK 的 toml 特例、Godot 启动器不要当通用层抄。

---

## 1. 真机约束（已验证）

掌机：Anbernic Linux ARM64，IP 由 DHCP 分配（最近验证为 `172.16.7.25`），Dropbeak agent **0.6.4**，端口 `8080`。
Ports 路径：**`/mnt/mmc/Roms/ports`**（不是 `/mnt/sdcard`）。  
UI：`dmenu.bin` / `launcher.sh`，**不是** Weston。GPU：**Mali-G31**。系统 SDL：**2.0.12**（构建容器 `pkg-config` 报 **2.0.10**，同一代，必须链这份，不要 bundled 新 SDL）。

洞窟物语（`doukutsu-rs`）在容器 `nostalgic_faraday`（`ubuntu:20.04` aarch64）里用 **系统 libsdl2** 编过，并已在这台掌机 Ports 菜单跑通。说明 **系统 SDL + Mali-G31 可用**。失败点在 unityloader 怎么用 EGL，不在 GPU 本身。

真机 `unity2021` 从菜单启动的日志：已经打印 `OpenGL ES 3.2` / `ARM` / `Mali-G31`，随后进程 **exit 1**。SDL 建窗成功，崩在之后 Unity 走真 EGL。

### 1.1 必须怎么链 SDL

| 项 | 要求 |
|---|---|
| SDL 来源 | `pkg-config` 链系统 `libsdl2`，**禁止** bundled / 静态新 SDL |
| 版本 | 对齐 2.0.10/2.0.12，避开 2.0.18 才有的 API |
| GPU | 走系统已能工作的 Mali / fbdev；**禁止** `eglQueryDevicesEXT` 一类设备枚举 |
| 依赖 | `libsdl2-dev`、`libasound2-dev`、`libudev-dev`、EGL/GLES 头、`libxext-dev`、`libbsd-dev`、`libmd-dev`、`pkg-config` |

洞窟物语对照命令（只作 SDL 策略对照，**不要**在该容器编 unityloader）：

```bash
source /root/.cargo/env
cargo build --release --locked --bin doukutsu-rs --no-default-features \
  --features "default-base,backend-sdl,render-opengl,exe,webbrowser,discord-rpc,sdl2-system,sdl2-minimum"
```

### 1.2 启动方式

- **必须**从 Ports 菜单点启动。`dmenu` 占着 framebuffer，Dropbeak CLI `exec` 拉游戏会失败。
- 先 `mount -o remount,exec /mnt/mmc`，不要把主程序拷到 `/tmp` 绕 `noexec`（会走 KMSDRM，和菜单抢屏）。
- 停游戏只 `kill` `unityloader`，不要 `shutdown` agent。
- PowerShell 会展开 `$HOME`、`$!`、`$WAYLAND_DISPLAY`。远程命令用**单引号**。`docker exec ... bash -c "...$HOME..."` 同样会被 PS 展开。

---

## 2. 为什么不是「换编译选项就能跑」

NEO 在 Weston / 较新 SDL 的掌机上能跑，不代表 Anbernic 能跑。根因在 `thunks/egl_sdl/egl_sdl.cpp`（**下列 1–7 是当时诊断，现已按 §3.2 / §3.3 落地**）：

1. **窗口由系统 SDL 创建**（已经能打出 GLES 3.2 / Mali-G31）。
2. Unity 随后按 **Android EGL** 调 `eglInitialize` / `eglQueryString` / `eglMakeCurrent`，传入的 `display` 是 Android 句柄，**不是** SDL 的 `egl_display`。
3. 默认路径把这些调用 **转发给真 `libEGL.so`**。Mali 上会踩 `eglQueryDevicesEXT`（洞窟物语笔记里会炸的接口）。
4. 源码已有 `#ifdef FAKE_EGL`（`eglInitialize` 返回 1.4、`eglCreateWindowSurface` 返回 `0xDEAD`）。Anbernic 构建 `BD_FAKE_EGL=ON`。开了之后若仍把 `0xDEAD` 交给真 `eglMakeCurrent`，一样崩——所以 FAKE 路径不再转发。
5. `getProc()`：`FAKE_EGL` 下禁止 `dlopen("libEGL.so")`，避免漏出设备枚举扩展。
6. 建窗：`SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN`；mali-fbdev + dmenu 时可能失败，需要降级 `FULLSCREEN_DESKTOP` / 窗口化。Anbernic GLES 只试 3.2 → 3.1。
7. `fatal_error` 必须 `abort()`，否则失败后带着空指针继续跑，日志像「打印完 GL 信息就莫名退出」。

CMake 原先 `target_link_libraries(... SDL2 ...)`，在 GlES_Dev（无 mold、GCC 9、SDL 2.0.10）上应对齐 `pkg-config sdl2`。NEO 自带的 `Dockerfile.builder` 是 Ubuntu 22.04 + mold，**不要**用那条线编给这台 Anbernic。

---

## 3. 改造要点（按优先级）

基线源码来自 NEO `2021.3`。下列改动才是 Anbernic 工作；ELF64/IL2CPP 加载器本身不要重写。

### 3.1 CMake（构建对齐系统 SDL）

- `USE_MOLD` 默认 **OFF**（GlES_Dev 无 mold，GCC 9）。
- `find_package(PkgConfig)` + `pkg_check_modules(SDL2 REQUIRED sdl2)`，链接 `${SDL2_LIBRARIES}`。
- **不要**把 `${SDL2_INCLUDE_DIRS}` 加进 include path：Ubuntu 的 cflags 是 `-I/usr/include/SDL2`，而源码写 `#include "SDL2/SDL.h"`，加上会变成 `SDL2/SDL2/SDL.h`。编译选项只用 `${SDL2_CFLAGS_OTHER}`（例如 `-D_REENTRANT`）。
- `option(BD_FAKE_EGL ... ON)` → `target_compile_definitions(... FAKE_EGL BD_FAKE_EGL)`。
- 真机迭代构建打开 `-DBD_ENABLE_LOG=ON -DBD_ENABLE_TRACE=ON`（TRACE 依赖 LOG，CMake 会强制）。
- 继续 `-static-libstdc++ -static-libgcc`（掌机 libstdc++ 可能偏旧）。`libbsd` / `libmd` 静态链，避免 ROM 上缺 `.so`。

NEO vendor 树里 **CMake 这一段已经改过一半**（pkg-config、USE_MOLD OFF、BD_FAKE_EGL）。合进本目录后以那份为准再补全。

### 3.2 `thunks/egl_sdl/egl_sdl.cpp`（核心，不止开宏）

| 项 | 要改成什么 |
|---|---|
| `getProc` | 黑名单：`eglQueryDevicesEXT` 及同族 Device/Platform Display 扩展，直接返回 NULL。`FAKE_EGL` 下 **禁止** `dlopen("libEGL.so")`。 |
| `eglInitialize` | 已有 FAKE 桩（返回 1.4）。保持。 |
| `eglCreateWindowSurface` / `eglCreateContext` | FAKE 返回 `0xDEAD`。保持。 |
| `eglQueryString` | FAKE 下返回固定短串，不把 Unity 的 Android display 交给真 EGL。 |
| `eglMakeCurrent` | FAKE 下只走 `bd_sdl_gl_make_current`：先 `SDL_GL_MakeCurrent`，失败则 `MakeCurrent(NULL)` 再绑回。Mali 上 Unity 着色器线程会 `EGL_BAD_ACCESS`，不重试会 GLSL link 失败、主线程 SIGBUS 黑屏。swap 也走同一路径。 |
| `eglGetConfigAttrib` | FAKE 下按 Unity 常见查询返回合理值，不转发真 EGL。 |
| 建窗 | `SDL_WINDOW_FULLSCREEN` 失败则 `FULLSCREEN_DESKTOP`，再失败则普通窗口。 |
| GLES | Anbernic 只试 **3.2 → 3.1**，禁止落到 GLES2（否则 Unity 会按 ES2 选 renderer）。 |
| vsync | `SDL_GL_SetSwapInterval(0)`；`eglSwapInterval` 无论请求多少都强制 0（mali-fbdev）。 |
| `eglGetDisplay` | FAKE 下不 `dlopen("libEGL.so")`。SDL 建窗成功后哨兵 display/context/surface，GL 符号走 `SDL_GL_GetProcAddress`。 |

空洞骑士须关 gfx jobs / HDR（`gamedata/assets/bin/Data/boot.config`），否则独立线程编 shader 更容易踩 `MakeCurrent` 失败。

### 3.3 `fatal_error` 必须停下来（已做）

`platform/common/logging.h` 的 `fatal_error` 打印后 `fflush` + `abort()`。`BD_LOG` 同样 `fflush`，避免崩溃前日志留在缓冲里。

### 3.4 启动脚本与部署布局

对齐已在真机上的 `unity2021`（脚本在 `D:\Locke\gitee\dropbeak\examples\deploy\unity2021.sh`，LF）：

```text
/mnt/mmc/Roms/ports/unity2021.sh
/mnt/mmc/Roms/ports/unity2021/
  unityloader          ← 每次迭代只换这个
  unity.toml
  gamecontrollerdb.txt
  unityloader.d/       ← 只放当前游戏需要的 Plugin ABI 匹配插件
  gamedata/            ← APK 的 assets/ + lib/，推一次
    lib/arm64-v8a/     ← libmain / libunity / libil2cpp / lib_burst_generated
    assets/bin/Data/
  log.txt
```

日常验证用 **`hk`**（见文首「现状」），不要再用旧 PortMaster 项 `K_空洞骑士[中].sh`。那份 `.sh` 是 PortMaster 模板（`source control.txt`、`CFW=Loong` 的 Weston 假设），**这台 Anbernic 对不上**。

`hk.toml` 要点：`displayWidth/Height=640x480`，`dpad_synthesize_hat=false`，`textureMaxDim=512`，`[game_patches.hollow_knight_viewport]` 全开，`[audio] backend="auto"`。  
`hk.sh` 每次启动合并 PlayerPrefs（`VidOSSet=1` 等，否则 Overscan 引导卡住进关）。插件目标：`cmake --build build-anbernic --target plugin_hollow_knight_viewport`（`EXCLUDE_FROM_ALL`）。

Samurai II 的 Madfinger/Google Play 离线兼容全部位于
`unityloader.d/samurai2_offline.so`，不在 loader 核心中。其配置必须显式启用：

```toml
[input]
controller_name = "Microsoft X-Box 360 pad"

[input.remap]
guide = "ESCAPE"

[google_play]
offline = true
```

游戏目录只部署需要的插件，例如 Hollow Knight 使用
`hollow_knight_viewport.so`，Samurai II 使用 `samurai2_offline.so`。插件与
`unityloader` 必须由同一 Plugin ABI 源码构建。

### 3.5 之后才轮到的（按新游戏日志补）

按日志补，不要预写一堆桩：

| 日志 | 动作 |
|---|---|
| `FindClass` / `GetMethodID` 失败 | 补 `javastubs/`（UnityPlayer / Activity / 更高 SDK） |
| `undefined symbol` / PLT stub | 补 `thunks/libc`、`thunks/ndk`、GLES 桥 |
| 音频静音 | 保持 `BD_ENABLE_OPENSLES_SHIM=OFF`（开了 FMOD 会卡死 OpenSL、不回退 AudioTrack） |
| Unity 2022 相对 2021 | GameActivity / Swappy / 个别 JNI；NEO 的 `legacy` 入口对不少 2021/2022 包仍够用 |
| `printStatistics` | 崩溃或 `atexit` 写成文件再 `pull`，得到 FakeJni 签名草稿，**不是**能跑的实现 |

新游戏仍建议先用 **Unity 2021.3.45 IL2CPP ARM64、强制 GLES、Input Manager (Old)** 空场景对照 GLES，再上完整包。

### 3.6 系统音量（已验证；多品牌 `auto`）

Anbernic **不是** Pulse，也不是 ALSA mixer 被旋钮改掉：

- `/etc/asound.conf` 的 `pcm.!default` 用 `ctl_elems` **每次 open 把 `digital volume` 锁成 63**。
- dmenu 系统音量写在 `/sys/class/power_supply/axp2202-battery/openbor_volume`（0–10），由 `volumeCtrl.dge`（tinymix）维护。
- 游戏 PCM 满幅度；若不做软件增益，系统音量对游戏无效。游戏中音量键会被 SDL 吃掉，CFW 守护进程也不在。

实现（审查结论：软件增益管道通用，音量来源必须可探测，不能写死 Anbernic）：

| 文件 | 作用 |
|---|---|
| `platform/common/sys_volume.cpp` | `auto`：SDL 走 Pulse/PipeWire → 不软件衰减、不截获音量键；否则探测 `openbor_volume` → sysfs 增益；都没有 → PCM 100%，键交给系统/游戏 |
| `projects/unityloader/javastubs/fakefmod.cpp` | `SDL_QueueAudio` 前按百分数缩放 **S16** PCM（仅这条 FMOD 回退路径） |
| `platform/common/input_backend.cpp` | 仅当 `bd_sys_volume_owns_keys()` 时吞掉 `VOLUMEUP`/`DOWN`（含 KEYUP），不发给游戏 |
| `hk.sh` | 无 Pulse 时 `SDL_AUDIODRIVER=alsa`；**仅当** `/proc/asound/cards` 有 `audiocodec` 才设 `AUDIODEV=plughw:audiocodec` |
| `hk.toml` `[audio]` | `backend="auto"`；可改 `sysfs` / `software` / `passthrough` |

覆盖：toml `sysfs_path` / `sysfs_max` / `intercept_volume_keys`，或环境变量 `BD_SYS_VOLUME_PATH`、`BD_SYS_VOLUME_MAX`、`BD_SYS_VOLUME_BACKEND`。  
`AudioManager.getStreamVolume` 仍是 stub 返回 100，响度只靠 fakefmod 增益。

**已知限制（换机时）：**

- 软件增益只作用在 fakefmod；若打开 `BD_ENABLE_OPENSLES_SHIM`，FMOD 会以为 OpenSL 可用然后静音，增益也走不到。
- 线性 `sample * pct / 100`，不是 dB；Anbernic 0–10 与系统观感已对齐，别的机子若用对数 mixer 可能听感不同。
- 音量键若被映射成手柄键而不是 `SDL_SCANCODE_VOLUMEUP/DOWN`，不会被拦截。
- 没有 ALSA mixer 后端：mixer 已被 `asound.conf` 锁死、音量又写在**未知路径**时，需要填 `sysfs_path`，否则会保持 100% PCM。
- `auto` 以 `SDL_AUDIODRIVER` 是否为 `pulse`/`pipewire` 判断 Pulse，避免残留 Pulse 套接字把 Anbernic 的 sysfs 盖掉。强制 `intercept_volume_keys=true` 叠在 Pulse 上会双重衰减。

---

## 4. 容器：只在 GlES_Dev 构建

| 容器 | 角色 |
|---|---|
| **`GlES_Dev`** | Dropbeak 构建 + **编 unityloader**。`ubuntu:20.04` aarch64，镜像 `dropbeak-gles-dev:local`。无 bind mount。x86 上的 **QEMU 用户态 ARM64**。 |
| **`nostalgic_faraday`** | 洞窟物语构建。cargo 在 `/root/.cargo/bin`。SDL **2.0.10**（`libsdl2-dev 2.0.10+dfsg1-3`）。**不要在这里编 unityloader。** 只当 apt 包对照。 |

### 4.1 对齐 nostalgic_faraday 的开发包

已在 GlES_Dev 装过并对过，确认输出过 `ALIGN_OK`、`pkg-config --modversion sdl2` → `2.0.10`：

`build-essential`、`cmake` 3.16.3、`ninja-build`、`g++-9`、`pkg-config`、`libsdl2-dev`（2.0.10）、egl/gles、alsa、udev、xext、`libbsd-dev`、`libmd-dev`。

若容器被重建，对照 `nostalgic_faraday` 的 `dpkg -l` 再装一遍，以 `pkg-config --modversion sdl2` = `2.0.10` 为准。

### 4.2 容器里不要测画面

GlES_Dev 是 QEMU，没有 Mali。在容器里跑 unityloader 会在 `eglQueryString` 乱码处 SIGTRAP。**只能交叉编译**，图形验证只在真机 Ports 菜单做。

---

## 5. 构建步骤（Windows 开发机）

合进 NEO 源码并改完 §3 之后。

日常迭代（带日志，体积约 110MB）：

```powershell
# 1. 源码拷进容器（GlES_Dev 无挂载）
docker cp D:\Locke\gitee\Bogodroid GlES_Dev:/workspace/Bogodroid

# 2. 容器内配置 + 编译（在 PowerShell 里整段用单引号，避免 $ 被展开）
docker exec GlES_Dev bash -c 'set -e
cd /workspace/Bogodroid
cmake -S . -B build-anbernic -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DUSE_MOLD=OFF \
  -DBD_FAKE_EGL=ON \
  -DBD_ENABLE_LOG=ON \
  -DBD_ENABLE_TRACE=ON
cmake --build build-anbernic -j$(nproc)
'

# 3. 产物拷回本机
docker cp GlES_Dev:/workspace/Bogodroid/build-anbernic/unityloader D:\Locke\gitee\Bogodroid\unityloader
```

本阶段 **Release**（无调试符号、无 BD_LOG/TRACE；CMake 对 Release 开 LTO + `-Wl,-s`）。2026-09-01 产物约 **4.5MB**（对照 RelWithDebInfo ~110MB），`readelf -S` 无 `.debug*`：

```powershell
docker exec GlES_Dev bash -c 'set -e
cd /workspace/Bogodroid
cmake -S . -B build-anbernic-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_MOLD=OFF \
  -DBD_FAKE_EGL=ON \
  -DBD_ENABLE_LOG=OFF \
  -DBD_ENABLE_TRACE=OFF \
  -DBD_ENABLE_OPENSLES_SHIM=OFF
cmake --build build-anbernic-release -j$(nproc)
'
docker cp GlES_Dev:/workspace/Bogodroid/build-anbernic-release/unityloader D:\Locke\gitee\Bogodroid\unityloader
```

验收构建：

- ARM aarch64 ELF（`readelf -h` 的 `Machine: AArch64`）
- `ldd` 链的是 `libSDL2-2.0.so.0`，**没有**把整份 SDL 静态编进 ELF
- cmake 配置日志有 `SDL2 2.0.10` 和 `BD_FAKE_EGL on`
- Release：`readelf -S` 没有 `.debug` 段

NEO 的 `BUILD-DOCKER.md` / `Dockerfile.builder`（Ubuntu 22.04 + mold）**不要**用于这台 Anbernic。

---

## 6. 部署与真机测试

CLI：`D:\Locke\gitee\dropbeak\dist\dropbeak-cli.exe`（或该仓库当前产物路径）。

```powershell
$deviceIp = "172.16.7.25" # 示例；先确认掌机当前 DHCP 地址
$env:DROPBEAK_HOST = $deviceIp
$cli = "D:\Locke\gitee\dropbeak\dist\dropbeak-cli.exe"
$hb = @("--host", $deviceIp, "--port", "8080")

& $cli ping @hb

# 游戏在跑时 8080 常连不上：先退回 Ports，或：
& $cli exec 'killall unityloader 2>/dev/null; true' @hb

# loader 约 110MB，必须 --chunk；标志写在子命令后面
& $cli push D:\Locke\gitee\Bogodroid\unityloader /mnt/mmc/Roms/ports/hk/unityloader --force --chunk --progress @hb
& $cli push D:\Locke\gitee\dropbeak\examples\deploy\hk.toml /mnt/mmc/Roms/ports/hk/hk.toml --force --progress @hb
& $cli push D:\Locke\gitee\dropbeak\examples\deploy\hk.sh /mnt/mmc/Roms/ports/hk.sh --force --progress @hb
& $cli exec 'chmod a+x /mnt/mmc/Roms/ports/hk.sh /mnt/mmc/Roms/ports/hk/unityloader; sed -i "s/\r$//" /mnt/mmc/Roms/ports/hk.sh /mnt/mmc/Roms/ports/hk/hk.toml' @hb
```

然后 **在掌机 Ports 菜单点 `hk`**。不要用 CLI `exec` 当前台游戏。PowerShell 远程命令用**单引号**，避免 `$` 被展开。

拉日志：

```powershell
& $cli pull /mnt/mmc/Roms/ports/hk/log.txt .\log.txt @hb
```

音量路径是否生效：日志里应有 `volume backend=sysfs` 和 `SDL_AUDIODRIVER=alsa AUDIODEV=plughw:audiocodec`。

大目录（整包 gamedata）不要走 `dropbeak push`：会 tar 进内存，且 CLI 没有 `--timeout`，默认 HTTP 30s。改用：

```powershell
tar -cf - -C <本地目录> . | curl --max-time 300 -X POST "http://${deviceIp}:8080/api/v1/files/extract?path=/mnt/mmc/Roms/ports/unity2021" --data-binary @-
```

（agent `ReadTimeout` 300s。具体 query 以当时 agent 版本为准。）

### 6.1 阶段判活

| 阶段 | 做什么 | 成功标准 |
|---|---|---|
| 0 通道 | ping、push 单文件 | agent 可达；`/mnt/mmc` 可 exec |
| 1 构建 | GlES_Dev 编出 ARM64 ELF | `file`/`ldd`/`pkg-config` 见 §5 |
| 2 SDL 窗口 | Ports 菜单启动 | `log.txt` 有 video driver、窗口尺寸；有 `OpenGL ES` / `Mali-G31` |
| 3 假 EGL | Unity 调 `eglInitialize` 等 | **不再** exit 1；没有把 Android display 交给真 `libEGL`；没有 `eglQueryDevicesEXT` |
| 4 第一帧 | `nativeRender` + swap | 画面出现；进程仍在 |
| 5 输入/音频 | 手柄、系统音量、游戏内音量键 | 日志 `volume backend=`；fakefmod 非静音 |
| 6 2022 差异 | 按 JNI/PLT 日志补桩 | `FindClass` 不再致命 |

空洞骑士已到阶段 5。`unity2021` 空场景仍可当 GLES 最小对照。

### 6.2 日志怎么读

- `volume backend=passthrough` 但 Anbernic 音量没变 → 没读到 `openbor_volume`，或 `SDL_AUDIODRIVER=pulse`。
- `OpenGL ES 3.2` / `Mali-G31` 然后立刻 `exited (1)` → 仍是 §3.2 的真 EGL 转发问题。
- `FindClass` / `GetMethodID` → Java 桩。
- `Unknown symbol` / reloc → thunk。
- 容器里 SIGTRAP / `eglQueryString` 乱码 → 忽略，那是 QEMU，不是真机结果。

---

## 7. 明确不要做的事

- 在 `nostalgic_faraday` 里编 unityloader  
- 用 NEO 的 Ubuntu 22.04 + mold / bundled SDL 编给这台机器  
- 在 GlES_Dev / QEMU 里当图形测试场  
- 用 CLI `exec` 代替 Ports 菜单启动游戏  
- 从本仓库当前 ELF32 HEAD「升级」到 2022（跳过 NEO）  
- 反编译空洞骑士 `unityloader`  
- 每次把整个 `gamedata/` 再 push 一遍  
- 把 loader 拷到 `/tmp` 绕 noexec  
- 先上 Vulkan / Play Asset Delivery / 完整商业包  
- 打开 `BD_ENABLE_OPENSLES_SHIM`（当前会让 FMOD 静音）  
- 指望 `printStatistics` 自动生成能跑的 Display/Window  

---

## 8. 关联参考目录

本机开发、对照、部署用到的目录与文件。改代码只动「本仓库」；其它路径只读对照，不要把游戏资源拷进 git。

### 8.1 源码与通道

| 用途 | 路径 |
|---|---|
| **本仓库（在此改代码）** | `D:\Locke\gitee\Bogodroid` |
| NEO 基线（合入前对照，`jenny92-tech` · `2021.3`） | `C:\Users\Administrator\Desktop\Jump2021\vendor\Bogodroid` |
| NEO 自带 Docker 构建说明（**不要**用于这台 Anbernic） | `C:\Users\Administrator\Desktop\Jump2021\vendor\Bogodroid\BUILD-DOCKER.md` |
| NEO 字符串抽取（确认空洞骑士 loader 就是 NEO） | `C:\Users\Administrator\Desktop\Jump2021\vendor\unityloader-input-strings.txt` |
| Dropbeak（推文件 / 拉日志，不是兼容层） | `D:\Locke\gitee\dropbeak` |
| Dropbeak 调查纪要（通道 + SDL 约束） | `D:\Locke\gitee\dropbeak\BOGODROID.md` |
| Dropbeak 使用说明（含 CLI 与 Ports） | `D:\Locke\gitee\dropbeak\docs\USAGE.md` |
| Dropbeak 架构 | `D:\Locke\gitee\dropbeak\docs\DESIGN.md` |
| 真机 Ports 启动脚本样例（LF） | `D:\Locke\gitee\dropbeak\examples\deploy\unity2021.sh`、**`hk.sh` / `hk.toml`** |
| Dropbeak CLI 产物 | `D:\Locke\gitee\dropbeak\dist\dropbeak-cli.exe` |

### 8.2 Jump2021：已跑通的 Unity 2021 布局（对照，不要当工作树）

| 用途 | 路径 |
|---|---|
| Jump2021 工程根 | `C:\Users\Administrator\Desktop\Jump2021` |
| 已用 NEO loader 跑 2021.3 的 Port 布局 | `C:\Users\Administrator\Desktop\Jump2021\portmaster\` |
| 掌机部署说明 | `C:\Users\Administrator\Desktop\Jump2021\portmaster\README-掌机部署.md` |
| 通用 `unity.toml` 样例 | `C:\Users\Administrator\Desktop\Jump2021\portmaster\Unity\unity.toml` |
| 该 Port 的 loader / gamedata 目录 | `C:\Users\Administrator\Desktop\Jump2021\portmaster\Unity\` |
| Unity **2021.3.45f2** IL2CPP ARM64 `gamedata`（真机 `unity2021` 用的那份） | `C:\Users\Administrator\Desktop\Jump2021\2021\portmaster\Unity\gamedata\` |
| 空洞骑士 Port 目录（unityloader 是 NEO 产物） | `C:\Users\Administrator\Desktop\Jump2021\hollowknight\` |
| 空洞骑士 toml / 启动器说明 | `C:\Users\Administrator\Desktop\Jump2021\hollowknight\hk.toml`、同目录 `README.md` |

空洞骑士 `.sh` 是 PortMaster 模板（Weston / `CFW=Loong`），**这台 Anbernic 对不上**，只对照 loader 与 `hk.toml`，不要抄启动脚本。

### 8.3 洞窟物语：系统 SDL 已在本机跑通（只对照 SDL 策略）

| 用途 | 路径 |
|---|---|
| 洞窟物语源码 | `D:\Locke\gitee\doukutsu-rs` |
| Anbernic 必须链系统 SDL 的原始笔记 | `D:\Locke\gitee\doukutsu-rs\doukutsu-rs构建环境.txt` |
| 构建容器（**只编 doukutsu-rs**，不要编 unityloader） | Docker 容器名 `nostalgic_faraday`（`ubuntu:20.04` aarch64） |

### 8.4 构建容器与真机

| 用途 | 路径 / 名称 |
|---|---|
| unityloader 构建容器 | Docker 容器名 **`GlES_Dev`**，镜像 `dropbeak-gles-dev:local`，`ubuntu:20.04` aarch64 |
| 容器内源码落点（无 bind mount，靠 `docker cp`） | `GlES_Dev:/workspace/Bogodroid` |
| 容器内构建目录 | `GlES_Dev:/workspace/Bogodroid/build-anbernic/`（迭代）· `build-anbernic-release/`（本阶段 Release） |
| 掌机 IP / agent | DHCP 地址（最近验证 `172.16.7.25:8080`，agent 0.6.4） |
| 掌机 Ports 根（不是 `/mnt/sdcard`） | `/mnt/mmc/Roms/ports` |
| 已部署的 Unity 2021 测试包 | `/mnt/mmc/Roms/ports/unity2021.sh` + `/mnt/mmc/Roms/ports/unity2021/` |
| 已部署的空洞骑士测试入口 | `/mnt/mmc/Roms/ports/hk.sh` + `/mnt/mmc/Roms/ports/hk/` |
| 已验证的插件目录 | `HollowKnight/unityloader.d/hollow_knight_viewport.so`、`Samurai2/unityloader.d/samurai2_offline.so` |
| 旧 PortMaster 空洞骑士（不要当启动器） | `/mnt/mmc/Roms/ports/` 下 `K_空洞骑士[中].sh` + `hollowknight/` |
| 已部署且菜单能跑的洞窟物语 | `/mnt/mmc/Roms/ports/` 下洞窟物语脚本 + `CaveStory+/` |

---

## 9. 下一步

空洞骑士 Anbernic 主路径已通。接着按需做，不要回头重写加载器：

1. 换其它 Linux ARM 掌机时先看 `log.txt` 的 `volume backend=`；不是 sysfs 就填 `[audio] sysfs_path` 或让 Pulse 管音量。  
2. `textureMaxDim` 对 ETC2/ASTC 目前不会缩小（压缩上传未拦截）。  
3. ALSA underrun 偶发，未修。  
4. 新游戏按日志补 `FindClass` / PLT / `ASensor*`，不要预写一堆桩。  
5. Anbernic loader 改动尚未提交；游戏资源不得进 git。
