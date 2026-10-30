# Bogodroid NEO — BrickGame 分支

> © 2025-2026 jenny92-tech &lt;jennyliu90223@gmail.com&gt; · GPL v3 · See [LICENSE.md](./LICENSE.md)

> **这个仓库在干什么**
> 把 Android 端 Unity IL2CPP（arm64）的原生库包进 ARM Linux 掌机跑通 —— 目标机是 **Anbernic H700** 与 **TrimUI Smart Pro**。
>
> | 想知道 | 看 |
> |---|---|
> | 怎么编 `unityloader`（容器、选项、产物检查） | **[BUILD-DOCKER.md](./BUILD-DOCKER.md)** |
> | 从解包到发版的完整流程 | **[docs/PORTING_PLAYBOOK.md](./docs/PORTING_PLAYBOOK.md)** |
> | 日志开关、Dropbeak 推文件、掌机启动 | **[AGENTS.md](./AGENTS.md)** |
> | 某款游戏做到哪一步 | `docs/<端口名>.md` |

---

This is an experimental project attempting to create a wrapper environment around native libraries from ARM Android Apps, with the intent of running the libraries (or entire apps and games) on ARM Linux handhelds. The current focus is on supporting the Unity game engine.

The project is heavily based on [JohnnyOnFlame/gmloader-net](https://github.com/JohnnyonFlame/gmloader-next) and [ChristopherHX/libjnivm](https://github.com/ChristopherHX/libjnivm).

The idea is to:
- load the libraries, patch them at runtime
- wrap, bridge or stub any required symbols from Android system libraries
- provide a JNI interface for communication with placeholder "java" classes
- provide implementations and stubs for common java and android classes
- ultimately wrap Audio, GLES and Input to host interfaces and libraries

This project does not aim to run your App out of the box. It is a framework and a set of working samples for your own porting project.

# 已移植 / 进行中的端口

只列有文档记录的；更细的过程、断言与未完成项在各文档里。

| 游戏 | 包名 | 状态 | 记录 |
|---|---|---|---|
| BrickGame Pro（肥鱼方块） | `com.perseusgames.brickgamepro` | 已发版，走完整条流水线 | [BRICKGAMEPRO.md](docs/BRICKGAMEPRO.md) · [TODO](docs/BRICKGAME_TODO.md) |
| Oddmar | `com.mobge.Oddmar` | 标题界面已渲染，输入/资源仍在收尾 | [ODDMAR.md](docs/ODDMAR.md) · [资源瘦身](docs/ODDMAR-ASSET-SLIMMING.md) |
| Blackmoor 2 | `com.fourfats.blackmoor2` | 进行中（流水线第二个样本） | [BLACKMOOR2.md](docs/BLACKMOOR2.md) |
| FiveHearts | `com.fivehearts.offline` | 可运行，视频桥已修 | [FIVEHEARTS.md](docs/FIVEHEARTS.md) |
| Space Squids | — | 掌机验证：触摸光标 + 原生震动 | [SPACESQUIDS_CONTROLLER.md](docs/SPACESQUIDS_CONTROLLER.md) · [HAPTICS.md](docs/HAPTICS.md) |

`projects/unityloader/plugins/` 里还有 `hollow_knight_viewport`、`skul_pad`、`samurai2_offline`、`terraria_autoname`、`oddmar_input`、`oddmar_video`、`oddmar_assetlocator` 等端口专用插件，约定见 [plugins/README.md](projects/unityloader/plugins/README.md)。

# License & Community Spirit

Bogodroid is licensed under **GNU GPL v3** (see [LICENSE.md](./LICENSE.md)). That license already does the heavy legal lifting: anyone who ships a binary derived from this code must publish their source under the same terms — there is no closed-source Bogodroid fork.

Beyond the license, this project exists to build a shared Unity-on-Linux-handheld ecosystem. We welcome forks, pull requests, port projects, and discussion — and we hope contributors stay because they value that community, not because the GPL forced them to. What we'd rather not see:

- Bogodroid bundled into paid products or services without contributing fixes back upstream.
- Closed builds that strip attribution or pretend the work is original.
- Forks that exist only to circumvent GPL obligations.

If you build something cool on top of Bogodroid, open a PR, file an issue, or just tell us about it. That is the contribution that keeps this project moving.

---

Bogodroid 使用 **GNU GPL v3** 协议（见 [LICENSE.md](./LICENSE.md)）。GPL 已经在法律层面保证：任何基于本项目分发的二进制都必须以同样协议公开源码——不存在闭源的 Bogodroid fork。

协议之外，本项目存在的目的是**共建 Unity-on-Linux 掌机生态**。欢迎 fork、PR、移植项目、技术讨论——更希望大家因为认同社区精神留下来，而不是被 GPL 强制约束。我们不希望见到的做法：

- 把 Bogodroid 打包进收费产品或服务但从不回馈修复。
- 闭源构建里抹去署名、把工作伪装成原创。
- 单纯为了规避 GPL 义务而搞的 fork。

如果你基于 Bogodroid 做了有趣的东西，欢迎 PR、提 issue、或者直接告诉我们。这就是让项目持续前进的真正贡献。

# Commit Message Convention

All new commits in this repository must use English subjects and the existing
`scope: summary` format:

```text
<scope>: <imperative summary>
```

Rules:

- Use a lowercase scope that names the affected component or workflow.
- Write the summary in English, imperative mood, with no trailing period.
- Keep the subject line to 72 characters when practical; put extra context in
  the commit body.
- Do not mix Chinese and English in the subject line. If Chinese context is
  necessary, put it in the commit body instead.
- Prefer one logical change per commit.

Common scopes include `build`, `ci`, `cmake`, `docs`, `tools`, `logging`,
`unityloader`, `jnivm`, `javastubs`, `graphics`, `audio`, `input`,
`thunks/libc`, `thunks/zlib`, and `thunks/egl_sdl`. Add a new scope only when it
matches a real component, tool, or workflow in the tree.

Examples:

```text
unityloader: add plugin host and Android compatibility stubs
build: ignore generated build directories
tools/unity_astc: validate audio resource metadata
```

# How to Build

构建在 `GlES_Dev` 容器里做（Ubuntu 20.04 + `aarch64-linux-gnu-g++` 9.4 + FFmpeg dev），宿主机不需要装交叉工具链。完整说明、源码同步方式和产物自检见 **[BUILD-DOCKER.md](./BUILD-DOCKER.md)**；最短路径：

```bash
docker cp projects/unityloader/. GlES_Dev:/workspace/Bogodroid/projects/unityloader/
docker exec GlES_Dev bash -lc '
  cd /workspace/Bogodroid &&
  cmake -S . -B build-device-release -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBD_ENABLE_LOG=OFF -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF \
    -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF &&
  cmake --build build-device-release -j"$(nproc)" --target unityloader &&
  aarch64-linux-gnu-strip build-device-release/unityloader
'
```

三个开关别省：`BD_ENABLE_LOG=OFF`（发版不带日志）、`JNIVM_ENABLE_RETURN_NON_ZERO=OFF`（CMake 缓存陷阱，见 [AGENTS.md](./AGENTS.md)）、`-DCMAKE_BUILD_TYPE=Release`（否则是 `-O0`，跑起来像卡）。

> 上游那份「在 Linux 主机上 `apt install` 后直接 `cmake ..`」的原生路线不适用于本仓库：产物是宿主架构，而且要额外照顾 FFmpeg / SDL2 / libbsd 的 arm64 头文件。仓库里的 `scripts/build-docker.sh`、`scripts/unity6/build-{rel,log}.sh` 是这条旧路线的残留，**已废弃**（原因见 BUILD-DOCKER.md §5），现在运行会在开头直接报废弃并 `exit 1`。

### Building a Specific Project

By default the build compiles the `unityloader` project. To build another one (e.g. `hexagonloader`), pass `-DPROJ` at configure time:

```bash
cmake -S . -B build-hexagon -DPROJ=hexagonloader -DCMAKE_BUILD_TYPE=Release
```

### Unityloader Plugins

Game-specific compatibility code is built as shared-library plugins and deployed beside a game's TOML file in `unityloader.d/`. Build all bundled plugins with `--target unityloader_plugins`, or a single one with `--target plugin_<name>` (the artifact is `<name>.so`). See [`projects/unityloader/plugins/README.md`](projects/unityloader/plugins/README.md) for the Plugin ABI, lifecycle, configuration and deployment layout.

# Running

`unityloader` uses a TOML config file. Examples live in `configs/`; after extracting a game's APK into `gamefiles/unity/` you can run it as-is:

```bash
./unityloader ../configs/unity.toml
```

Toml 里只有必要项：`[debug]` / `[device]` 整表都可以不写（日志间隔默认 2000 ms，分辨率自动探测）。掌机侧部署（Dropbeak 推送、启动脚本、`LD_LIBRARY_PATH`、日志判据）见 [AGENTS.md](./AGENTS.md) 与 playbook §1.7。

# 调试与取证

**JNI tracing（需要 root 过的 Android 设备 + ADB + Python）**

1. [Install a Frida server on your Android device](https://frida.re/docs/android/)
2. `pip install jnitrace`
3. `jnitrace -o jnitrace.json com.example.myapplication`
4. Press CTRL-C to finish acquiring data
5. `python tools/jnitrace_parse.py <input.json> <output.json>` gives a consolidated tree of the classes, methods, fields and signatures that were touched
6. `tools/unity_traces` holds a trace of a minimal Unity game starting up

**IL2CPP trace logging**

1. Locate your copy of il2cpp from the Unity Editor under `Data/il2cpp`
2. Copy that folder into `projects/unityloader/`
3. `cd tools/ && python3 gen_il2cpp_log_shim.py`
4. Configure with `-DIL2CPP_TRACE=ON -DBD_ENABLE_LOG=ON`（子开关依赖主开关，否则 CMake 直接 `FATAL_ERROR`）

**容器回放取证**：不接掌机也能跑一局并截图/断言，参数与判定见 playbook §1.6；Unity 6 的运行镜像与逐帧脚本见 [UNITY6.md](docs/UNITY6.md)。

# 资产工具

- **[tools/unity_astc/](tools/unity_astc/README.md)** — bundle 瘦身主流程：ASTC 重分档、音频重编码、资源校验。判据是**运行时 RSS**（UMA 内存），不是磁盘体积。
- [tools/dll_surgery/](tools/dll_surgery/README.md) — 替换 mono/il2cpp 侧原生库的样例。
- [tools/godot_pck/](tools/godot_pck/README.md) — Godot `.pck` 处理。
- `tools/fonts`、`tools/steam_mock`、`tools/tests` — 字体替换、Steam API 假实现、工具自测。
