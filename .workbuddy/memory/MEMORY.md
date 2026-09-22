# Bogodroid-anbernic — 项目长期记忆

## 项目是什么

把 Android 端 Unity IL2CPP（arm64）原生库包进 ARM Linux 掌机运行
（`gmloader-net` + `libjnivm`）。每个游戏 = `projects/<name>/` + 共享 `libjnivm/`、
`thunks/`、`platform/`、根 `javastubs/`。

仓库有两个 worktree：本目录在 `video` 分支；`D:/Locke/gitee/Bogodroid` 在 `unity6`。

## 构建 / 测试环境（硬性）

- **只用 `GlES_Dev` 容器**（`dropbeak-gles-dev:local`，aarch64 Ubuntu 20.04 + g++ 9.4）。
  `dropbeak-gles-dev:local` **镜像里没有** g++/cmake/ninja，工具链只在该容器可写层。
- 源码用 `docker cp` 单文件同步进 `/workspace/Bogodroid`，不要整树重推。
  **`docker cp` 保留 mtime → ninja 会说 "no work to do"，改完必须 `touch`。**
- 每次配置显式带 `-DJNIVM_ENABLE_RETURN_NON_ZERO=OFF`（CMake 缓存项，共用目录会被上次实验污染）。
- 构建目录分工：
  - `build-oddmar-verbose` — RelWithDebInfo、无 LTO、`-g`、LOG+VERBOSE 开（迭代诊断，≈2–4 min）
  - `build-oddmar` — Release + LOG=ON + TRACE/VERBOSE OFF（上机中间态）
  - `build-regress` — RelWithDebInfo + LOG=ON + TRACE/VERBOSE OFF（回归对照用）
- 运行脚本固定要显式传 `BOOT_LOADER=`，否则默认跑 `/workspace/Bogodroid/build-oddmar`（老二进制）。
- 本机 Bash 需 `export PATH="/usr/bin:/bin:/c/Windows/System32:/c/Users/Administrator/.workbuddy/binaries/PortableGit/versions/1.2.0/usr/bin:$PATH"`。

## 日志规则（踩过的大坑）

- `jnivm_log_enabled(tag, format)` 看的是 **tag** 是否以 `BD-` 开头，**不是** format。
  自定义诊断一律 `LOG("BD-XXX", ...)`，否则被静默丢弃。
- 全关（`BD_ENABLE_LOG=OFF`）只用于压体积/对照性能；深挖再临时开 TRACE/VERBOSE。
- `fatal_error` / SEGV 回溯**不依赖** `BD_ENABLE_LOG`。

## Oddmar（当前在手）

- **目标：断网单机运行**（不接 Google Play / 内购 / 在线服务）。
  billing、Firebase、Play Services 类缺失桩**不需要功能正确**，只要不阻塞、不致命。
- 排查顺序固定：**资源加载 → 场景挂载 → 画面**。
- 交接文档：`docs/HANDOFF-ODDMAR.md`（先读它）。
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
