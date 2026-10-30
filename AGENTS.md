# Bogodroid Agent Notes

## 从哪开始（按处境选一个，别通读全部）

| 处境 | 读这个 |
|---|---|
| **要把一款 Unity 游戏从解包做到发版** | [`docs/PORTING_PLAYBOOK.md`](docs/PORTING_PLAYBOOK.md) — **唯一的通用流程文档**：止损判据、六段流水线、每段命令、断言脚本、硬坑清单、维护契约 |
| 手上游戏做到一半，想知道进度 | `docs/` 下的端口记录：[`BRICKGAMEPRO.md`](docs/BRICKGAMEPRO.md)（已走完全流程）、[`BLACKMOOR2.md`](docs/BLACKMOOR2.md)（进行中） |
| 目标是 Unity **6000.x** | 先 [`docs/UNITY6.md`](docs/UNITY6.md)（玩家类搬家、`initJni` 签名、主线程 `ALooper`、`Class.forName` 二进制名归一化），再回 playbook |
| 只是改一个按钮 / 一个画面参数 | 直接跳 playbook §4 插件约定 → §1.6 容器回放，别从头读 |

> **规范只在 `PORTING_PLAYBOOK.md` 里**，不放"某款游戏做到哪"的过程；过程进 `docs/<端口名>.md`。
> playbook §0.4 有基线状态和"怎么判断本文是否过期"的两条命令，动手前先跑一遍。那两条只看**已提交**历史，`git status --short` 有改动时同样按"可能已过期"处理。

## 日志 / 构建开关（必读）

日志是**编译期**开关，不是运行时 toml。层次见 `platform/common/logging.h`：

| CMake | 默认 | 作用 |
|-------|------|------|
| `BD_ENABLE_LOG` | **OFF**（CMake 默认） | 主开关：`BD_LOG` / `[BD-MEM]` / 插件 `api->log` / jnivm `LOG`（关闭时 `[BD-MEM]` 那段整段编译移除，不再每 2 s 读 `/proc`） |
| `BD_ENABLE_TRACE` | OFF | 需 LOG：额外 `BD_DEBUG` / `BOOT_LOG` 等 |
| `BD_ENABLE_VERBOSE` | OFF | 需 LOG：大量 `verbose()`（含 NATIVE/JNI 刷屏） |
| `IL2CPP_TRACE` | OFF | 需 LOG：il2cpp 内部 trace |

**两个档位，别混**（构建都在 `GlES_Dev` 容器里做，源码副本 `/workspace/Bogodroid`，不是 bind mount）：

| 用途 | 构建 | 产物 |
|---|---|---|
| 发版 / 上机 | Release + **LOG/TRACE/VERBOSE/IL2CPP_TRACE 全 OFF** + `strip` | `build-device-release/unityloader`，约 5.7 MB |
| 排查 / 容器回放 | **RelWithDebInfo**（`-O2 -g`，符号可读）+ **`BD_ENABLE_LOG=ON`** + 不 `strip`（要 `BD_DEBUG` 再加 TRACE） | `build-anbernic-rel/unityloader`，约 90–168 MB，只临时上机 |

两个档位用**两个构建目录**：`cmake -S . -B` 会改写 `CMakeCache.txt`，共用一个目录会让发版包带上上一次的 `BD_ENABLE_LOG=ON`。

LOG 层（仅排查档）：`[BD-MEM]`、视频 `publish`/`swap`、codec 周期摘要、worker 启停、`[STUB-MISS]`；逐帧 upload/step/luma/blit 在 **TRACE**（`BD_DEBUG`）。
正式包判定：`log.txt` 里**零 `[BD-*]` 行**（崩溃时的 `[BD-SEGV]` 除外），有就是拿错了二进制。静态检查别用 `strings | grep '^\[BD-'` 判空——`[BD-SEGV]`、`[BD-J2]` 无条件编入，发版包也有；口径见 playbook §6.1。
`fatal_error` / SEGV 回溯**不依赖** `BD_ENABLE_LOG`。

toml 里的 `[debug] mem_log_interval_ms` **可省略**（默认 2000 ms；`BD_MEM_LOG_MS` 环境变量优先，`0` 关闭），整个 `[debug]` 表都可以不写。`[device]` 同理：`displayWidth/Height/RefreshRate` 省略或 `0` 即自动探测。铺配置只写必要项。

完整命令见 playbook §1.1；容器基线、源码同步与产物检查见 [`BUILD-DOCKER.md`](BUILD-DOCKER.md)；CMake 选项表以 `CMakeLists.txt` 的 `option()` 为准（[`BUILD-DOCKER.md`](BUILD-DOCKER.md) §3 有同样一份）。
`scripts/build-docker.sh` 与 `scripts/unity6/build-{rel,log}.sh` 属上游 x86 交叉路线，**已废弃**，别照它们编（原因见 BUILD-DOCKER.md §5）。

### `JNIVM_ENABLE_RETURN_NON_ZERO`（CMake 缓存陷阱，必读）

`libjnivm` 的这个选项决定 `[STUB-MISS]` 缺桩时返回什么：

- **OFF**（默认，上机必须）→ `null` / 0，Unity 自己 try/catch，最好情况只丢一个 NRE；
- **ON**（实验）→ 硬造 dummy 对象，Unity 把它 cast 成 `String`/`Throwable` → jnivm `Invalid Reference, Unexpected Type` → `terminate()` / `exited 134`。

它是 **CMake 缓存项**：共用构建目录（`build-unity6/`、`build-trimui-*/` 之类）时会被上一次实验遗留成 `ON`，之后即使只改无关代码，编出来的 `unityloader` 也会崩，且崩点看着落在完全不相关的桩上。**每次构建显式带 `-DJNIVM_ENABLE_RETURN_NON_ZERO=OFF`**，细节与判读方法见 playbook §1.2。

同类缓存项还有两个，已在 `CMakeLists.txt` 里 `FORCE` 固定，别再靠命令行覆盖：`JNIVM_ENABLE_DEBUG` **恒 ON**（不是日志开关：`JNI_DEBUG` 影响 `InternalFindClass()` 的类注册与嵌套类身份，也保留 `object is null` 诊断）；`JNIVM_ENABLE_TRACE` 跟随 `BD_ENABLE_LOG`。见 playbook §1.3。

## Dropbeak：大文件推送 / 拉取（掌机）

掌机 Dropbeak agent（默认 `http://<host>:8080`）对大 `.bundle` / `unityloader` 等文件有几条硬坑，按下面做。

**地址是 DHCP 分的，别抄文档里的 IP。** 端点表只维护在 playbook §1.7.1，命令里一律用 `$env:DROPBEAK_HOST` 或 `<DEV_HOST>` 占位。

### 已知问题

1. **CLI 默认 HTTP 超时约 30s**  
   `dropbeak-cli push/pull` 对大文件容易 `context deadline exceeded`，尤其是单次 PUT / 无 chunk 时。
2. **失败的上传可能留下残缺文件**  
   agent 侧可能已 `TRUNC` 再写；中途超时后远端 `ls -la` 大小会小于本地，**必须核对**。
3. **覆盖已存在文件必须 `force`**  
   否则返回 409；路径：`PUT /api/v1/files?path=<remote>&force=true`。
4. **错误 API 路径会 404**  
   正确是 `/api/v1/files?...`，不是 `/api/v1/fs/write`。

### 推荐做法（优先）

**≥50MB 推送：用 CLI chunk + force**（0.6.4+ 建议 `--chunk-size 16m --verify`；实测 ~311MB `ea9c` ≈45s）：

```powershell
$env:DROPBEAK_HOST = '<DEV_HOST>'   # 按环境改，见 playbook §1.7.1
& 'D:\Locke\gitee\dropbeak\dist\dropbeak-cli.exe' push `
  <local-file> <remote-abs-path> `
  --force --chunk --chunk-size 16m --verify --progress
# --verify 已做 SHA-256；仍可用 ls 再核对大小
& 'D:\Locke\gitee\dropbeak\dist\dropbeak-cli.exe' exec "ls -la <remote-abs-path>"
```

**中等文件（约几十～三百 MB）也可 curl 单次 PUT**（注意 `--max-time`，且用 `--upload-file`）：

```powershell
$remote = '/mnt/mmc/Roms/ports/Skul/...'
$uri = 'http://<DEV_HOST>:8080/api/v1/files?path=' +
  [uri]::EscapeDataString($remote) + '&force=true'
curl.exe -X PUT --max-time 7200 --upload-file <local-file> $uri `
  -w "`nhttp=%{http_code} size=%{size_upload}`n"
```

**大文件拉取：用 curl download**，勿依赖 CLI 默认超时：

```powershell
$uri = 'http://<DEV_HOST>:8080/api/v1/files/download?path=' +
  [uri]::EscapeDataString($remote)
curl.exe -L --max-time 7200 -o <local-out> $uri
```

### 资源约定（通用）

- **先改本地 staging，再推掌机**；推前用字节数（必要时 SHA256）确认与掌机一致。
- 禁止远程执行 Ports 游戏 `.sh` 做破坏性试验（除非用户明确要求）；覆盖文件用 `--force`。
- 缩纹理：`tools/unity_astc/`；UnityPy **磁盘体积常变大**，以运行时 `[BD-MEM] rss` / OOM 为准。

### 超时后检查清单

1. `dropbeak-cli ping` 是否通。  
2. 远端 `ls -la` 是否等于本地长度。  
3. 若偏小 → 当残缺文件处理，用 `--force --chunk` **整文件重推**，不要 resume 半成品。

## TrimUI Smart Pro：远程启动 / 停止（Oddmar 及同类 port）

掌机不需要手按，主机侧就能把 port 拉起来并收回。

### 拓扑

- 正式入口是 `Roms/PORTS/Oddmar.sh`（菜单由此进入），结尾直接前台跑 `$GAMEDIR/unityloader unity.toml`。
- `GAMEDIR` = `/mnt/SDCARD/Data/ports/Oddmar`：`control.txt` 里 `directory="mnt/SDCARD/Data"`，脚本再前缀一个 `/` 拼出来。

### 远程启动

固件常驻 `/usr/trimui/bin/runtrimui.sh`（由 `/etc/rc.d/S99runtrimui` 起）在循环里做这件事：**MainUI 退出后，若存在 `/tmp/cmd_to_run.sh` 就前台执行它，然后删掉它**。利用这点：

1. 把要跑的脚本先写到 SD 卡上 —— `/tmp` 不在 dropbeak 允许写入的路径里，直接 push 会 403；
   `cp <sdcard>/xxx.sh /tmp/cmd_to_run.sh && chmod +x /tmp/cmd_to_run.sh`
2. `killall -9 MainUI`，runtrimui 随后就会执行我们的脚本。
3. 脚本**必须前台 `exec` loader**，绝不能 `&` 后台：一旦后台，脚本立即返回，runtrimui 会马上把 MainUI 拉回来，表现为系统界面与游戏交替闪烁。
4. 停止：`killall -9 unityloader`。MainUI 由 runtrimui 自动拉起，不用管；`/tmp/cmd_to_run.sh` 会被同一轮循环删掉（清理是自动的）。

仓库里 [`docs/trimui_run.sh`](docs/trimui_run.sh) 是一份可参考的遥控入口脚本；Oddmar 的实机细节见 [`docs/ODDMAR.md`](docs/ODDMAR.md)。

### 必须自己设 `LD_LIBRARY_PATH`

`runtrimui.sh` 里是 **`export LD_LIBRARY_PATH=${SDCARD_TRIMUI_DIR}/lib`（覆盖式赋值，不追加）**，而 `/etc/ld.so.conf` 不存在、`/etc/ld.so.conf.d/` 是空的。当前 `unityloader` 不再把 FFmpeg 写入 ELF 的 `DT_NEEDED`；FFmpeg 由 Android media bridge 在首次使用视频时按需 `dlopen`。因此没有视频的 Unity 游戏不需要携带或配置 FFmpeg。

Oddmar 的视频仍使用 FFmpeg 4.2（`libav*.so.58`），其启动脚本必须显式把 `System/lib` 和 `Oddmar/ff58` 放入路径：

```sh
export LD_LIBRARY_PATH="/mnt/SDCARD/System/lib:$GAMEDIR/ff58${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

漏掉这行，Oddmar 播放视频时会因按需加载不到 FFmpeg 而无法解码；不使用视频的游戏不受影响。

### 验证画面

- `<PORT>/log.txt` 是正式脚本 `tee` 的落点；**LOG=OFF 的 build 里应只剩 SDL/EGL 原生噪声**（`SDL_UDEV_*`、`MALI_CreateWindow`、`OpenGL Renderer:`），**零 `[BD-*]` 行**。有 `[BD-*]` 就说明拿错了带 LOG 的二进制。
- 帧导出（`BD_DUMP_FRAME` / `BD_VIDEO_DUMP_FRAME`，默认关闭，说明见 [`docs/trimui_run.sh`](docs/trimui_run.sh) 注释）一次全开会留约 31 MB PPM，用完记得清。
- 部署后核对哈希：`sha256sum` 掌机文件 vs 本地构建产物，别只看大小。
- dropbeak `exec` 有约 30 s HTTP 超时：启动命令要后台化，或避免在里面长 `sleep`；大文件按上文 `--chunk` 走。
