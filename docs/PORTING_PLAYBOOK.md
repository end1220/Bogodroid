# Unity IL2CPP → Linux ARM 掌机移植 Playbook

面向 1GB 级 UMA 掌机（如 Anbernic BuildRoot）。新端口**先读本文**，大文件推送细节见根目录 [`AGENTS.md`](../AGENTS.md)。

## 0. 何时不要开坑（止损判据）

移植前用解包 + `tools/unity_astc/inventory_bundle.py` 粗估：

| 红灯 | 说明 |
|------|------|
| 标题/菜单阶段 Addressables 热包贴图+音频已近物理内存 | 进关再加载必顶穿 |
| 单个 bundle 同时塞「全角色/全音效/全 UI」 | 无法部分卸载；缩纹理只能延缓 |
| 运行时压 ASTC/ETC2 无效 | `textureMaxDim` **拦不住**压缩上传，必须离线 retier |

**负面案例：Skul（已止损）** — 标题预加载 RSS≈785MB / avail≈72MB，进 `gameBase` 后 avail≈7MB 假死。详见  
`D:\Locke\gitee\LinuxArmPorts\SKULL_ARM_LINUX_PORTING.md`。  
经验已提取进核心（`[BD-MEM]`、PAD 配置、present 插件回调）；**不要**在 Skul 上继续深挖标题 unload / 更深 ASTC，除非新游戏证明 ROI。

## 1. 标准流水线

```text
APK/解包 → 本地 staging 目录
    → inventory / astc_retier / shrink / audio_stream_patch
    → Docker 编 unityloader + 所需 plugin
    → Dropbeak push (--force --chunk --chunk-size 16m --verify)
    → 掌机手启游戏（勿远程 Skul.sh 一类）
    → 看 log：[BD-MEM] rss/sys_avail + 场景名
```

构建（示例）：

```powershell
docker run --rm --platform linux/amd64 `
  -v "D:\Locke\gitee\Bogodroid:/work" -w /work/build-aarch64 `
  bogo-builder:unity2017-armv7 `
  bash -c "cmake --build . -j2 --target unityloader plugin_<name>"
```

## 2. 资产工具矩阵

| 工具 | 用途 |
|------|------|
| `inventory_bundle.py` | 只读：贴图 VRAM / 音频 / 类型占比 |
| `astc_retier.py` / `retier_all.sh` | 强制 ASTC；`--packer original` 保兼容 |
| `shrink_bundle.py --cap N` | 降长边；磁盘可能变大，看 RSS |
| `audio_stream_patch.py` | LoadType；内嵌包加 `--allow-embedded --mode hybrid` |

优先看**运行时 RSS**，不要只看磁盘体积。

## 3. 核心能力（勿再写进标题插件）

| 能力 | 配置 / API |
|------|------------|
| 进程内存 | `[debug] mem_log_interval_ms`；日志 `[BD-MEM]` |
| PAD Java stub | `[play_asset_delivery] enabled/default_pack/pack_version/pack_path_template` |
| 每帧 present 钩子 | Plugin ABI v3：`register_present_callback`（见 `platform/common/plugin_present.h`） |
| CPU present / swap pause | 环境变量 `BD_EGL_CPU_PRESENT`、`BD_EGL_SWAP_PAUSE`（插件可 setenv） |

标题专属逻辑放 `unityloader.d/<game>.so`，见 [`projects/unityloader/plugins/README.md`](../projects/unityloader/plugins/README.md)。

## 4. 插件约定

1. 校验 `abi_version == BOGODROID_PLUGIN_ABI_VERSION`（当前 **3**）。
2. 需要每帧轮询时：`api->register_present_callback(...)`，不要导出供核心 `dlsym` 的游戏名符号。
3. PAD 路径补丁、IL2CPP hook 留在插件；核心只提供可配置 stub。

## 5. 复测清单

1. `dropbeak-cli ping`；大文件 `ls -la` 与本地一致。  
2. 日志：`[BD-MEM]` 在关键 `LoadScene*` 前后；`sys_avail` 是否见底。  
3. 若标题阶段 avail 已 &lt;100MB → **止损或换游戏**，不要赌进关后再省。
