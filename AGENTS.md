# Bogodroid Agent Notes

新端口先读 [`docs/PORTING_PLAYBOOK.md`](docs/PORTING_PLAYBOOK.md) 与 [`docs/CASE_STUDIES.md`](docs/CASE_STUDIES.md)。Skul 已止损。

## 日志 / 构建开关（必读）

日志是**编译期**开关，不是运行时 toml。层次见 `platform/common/logging.h`：

| CMake | 默认 | 作用 |
|-------|------|------|
| `BD_ENABLE_LOG` | **OFF** | 主开关：`BD_LOG` / `[BD-MEM]` / 插件 `api->log` / jnivm `LOG` |
| `BD_ENABLE_TRACE` | OFF | 需 LOG：额外 `BD_DEBUG` / `BOOT_LOG` 等 |
| `BD_ENABLE_VERBOSE` | OFF | 需 LOG：大量 `verbose()`（含 NATIVE/JNI 刷屏） |
| `IL2CPP_TRACE` | OFF | 需 LOG：il2cpp 内部 trace |

**上机默认**：`CMAKE_BUILD_TYPE=Release` + 上述全 OFF + `strip`（~5MB）。  
**排障**：临时 `BD_ENABLE_LOG=ON`（可加 TRACE/VERBOSE）重编推送；通了再改回关日志的 Release。  
`fatal_error` / SEGV 回溯**不依赖** `BD_ENABLE_LOG`。

完整命令见 playbook §1.1；CMake 细节见 [`BUILD-DOCKER.md`](BUILD-DOCKER.md) §5–6。

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
