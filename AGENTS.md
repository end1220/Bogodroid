# Bogodroid Agent Notes

新端口先读 [`docs/PORTING_PLAYBOOK.md`](docs/PORTING_PLAYBOOK.md) 与 [`docs/CASE_STUDIES.md`](docs/CASE_STUDIES.md)。Skul 已止损。

## 日志 / 构建开关（必读）

日志是**编译期**开关，不是运行时 toml。层次见 `platform/common/logging.h`：

| CMake | 默认 | 作用 |
|-------|------|------|
| `BD_ENABLE_LOG` | **OFF** | 主开关：`BD_LOG` / `[BD-MEM]` / 插件 `api->log` / jnivm `LOG`（关闭时 `[BD-MEM]` 那段整段编译移除，不再每 2 s 读 `/proc`） |
| `BD_ENABLE_TRACE` | OFF | 需 LOG：额外 `BD_DEBUG` / `BOOT_LOG` 等 |
| `BD_ENABLE_VERBOSE` | OFF | 需 LOG：大量 `verbose()`（含 NATIVE/JNI 刷屏） |
| `IL2CPP_TRACE` | OFF | 需 LOG：il2cpp 内部 trace |

**上机默认**：`CMAKE_BUILD_TYPE=Release` + 上述全 OFF + `strip`（~5MB）。  
**排障**：临时 `BD_ENABLE_LOG=ON`（可加 TRACE/VERBOSE）重编推送；通了再改回关日志的 Release。  
`fatal_error` / SEGV 回溯**不依赖** `BD_ENABLE_LOG`。

toml 里的 `[debug] mem_log_interval_ms` **可省略**（默认 2000 ms；`BD_MEM_LOG_MS` 环境变量优先，`0` 关闭），整个 `[debug]` 表都可以不写。`[device]` 同理：`displayWidth/Height/RefreshRate` 省略或 `0` 即自动探测。铺配置只写必要项。

完整命令见 playbook §1.1；CMake 细节见 [`BUILD-DOCKER.md`](BUILD-DOCKER.md) §5–6。

### `JNIVM_ENABLE_RETURN_NON_ZERO`（CMake 缓存陷阱，必读）

`libjnivm` 的这个选项决定 `[STUB-MISS]` 缺桩时返回什么：

- **OFF**（默认，上机必须）→ `null` / 0，Unity 自己 try/catch，最好情况只丢一个 NRE；
- **ON**（实验）→ 硬造 dummy 对象，Unity 把它 cast 成 `String`/`Throwable` → jnivm `Invalid Reference, Unexpected Type` → `terminate()` / `exited 134`。

它是 **CMake 缓存项**：共用 `build-aarch64/` 时会被上一次实验遗留成 `ON`，之后即使只改无关代码，编出来的 `unityloader` 也会崩，且崩点看着落在完全不相关的桩上。**每次构建显式带 `-DJNIVM_ENABLE_RETURN_NON_ZERO=OFF`**，细节与判读方法见 playbook §1.2；真实案例见 [`docs/CASE_STUDIES.md`](docs/CASE_STUDIES.md) 的 Maximus2 一节。

同类缓存项还有两个，已在 `CMakeLists.txt` 里 `FORCE` 固定，别再靠命令行覆盖：`JNIVM_ENABLE_DEBUG` **恒 ON**（不是日志开关：`JNI_DEBUG` 影响 `InternalFindClass()` 的类注册与嵌套类身份，也保留 `object is null` 诊断）；`JNIVM_ENABLE_TRACE` 跟随 `BD_ENABLE_LOG`。见 playbook §1.3。

## Dropbeak：大文件推送 / 拉取（掌机）

掌机 Dropbeak agent（默认 `http://<host>:8080`）对大 `.bundle` / `unityloader` 等文件有几条硬坑，按下面做。

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
$env:DROPBEAK_HOST = '172.16.7.55'   # 按环境改
& 'D:\Locke\gitee\dropbeak\dist\dropbeak-cli.exe' push `
  <local-file> <remote-abs-path> `
  --force --chunk --chunk-size 16m --verify --progress
# --verify 已做 SHA-256；仍可用 ls 再核对大小
& 'D:\Locke\gitee\dropbeak\dist\dropbeak-cli.exe' exec "ls -la <remote-abs-path>"
```

**中等文件（约几十～三百 MB）也可 curl 单次 PUT**（注意 `--max-time`，且用 `--upload-file`）：

```powershell
$remote = '/mnt/mmc/Roms/ports/Skul/...'
$uri = 'http://172.16.7.55:8080/api/v1/files?path=' +
  [uri]::EscapeDataString($remote) + '&force=true'
curl.exe -X PUT --max-time 7200 --upload-file <local-file> $uri `
  -w "`nhttp=%{http_code} size=%{size_upload}`n"
```

**大文件拉取：用 curl download**，勿依赖 CLI 默认超时：

```powershell
$uri = 'http://172.16.7.55:8080/api/v1/files/download?path=' +
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
