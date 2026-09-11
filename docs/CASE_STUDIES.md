# 案例：Skul（小骨）止损摘要

完整长文仍在 `LinuxArmPorts/SKULL_ARM_LINUX_PORTING.md`；**后续移植只读本节 + [`PORTING_PLAYBOOK.md`](PORTING_PLAYBOOK.md)**。

## 进行中：PC01（Five Hearts Under One Roof）

交接全文见 **[`PC01_HANDOFF.md`](PC01_HANDOFF.md)**（2026-09-11 19:05）。  
摘要：Unity 2022.3.62 IL2CPP；`InitFirebase`→`StartGame` 已通（两段 splash）；prefs `SettingData` **已 HIT** 但 `StartGame` 内仍 **NRE**；**无** `BundleManager.Initialization ENTER`。RSS≈650MB/972MB 为 **splash 基线**（UnityCache 未开）。Git 分支 **`pc01`**。

## 结论

- Unity 2020.3 IL2CPP + URP + Addressables/PAD；主界面可玩，**进关失败**。
- 1GB UMA：标题预加载后 RSS≈785MB / `sys_avail`≈72MB；`gameBase` 后 ≈821MB / ≈7MB → 假死或 137。
- 根因：**bundle 划分过粗**（热包同时塞 UI/角色/音频），不是单点 stub 能修。
- **已止损**；`skul_pad` 仅归档，默认 `enabled=false`。

## 可复用到核心的能力

| 能力 | 位置 |
|------|------|
| `[BD-MEM]` RSS / sys_avail | `platform/common/process_memory.h` |
| PAD Java stub 配置化 | `[play_asset_delivery]` + `javastubs/unity.cpp` |
| present 插件回调 ABI v3 | `register_present_callback` |
| 资产工具 | `tools/unity_astc/`（inventory / ASTC / audio_stream `--allow-embedded`） |
| 大文件推送 | 根目录 `AGENTS.md`（Dropbeak `--chunk --chunk-size 16m --verify`） |

## 踩坑清单（通用）

1. **`textureMaxDim` 压不了 ASTC/ETC2 上传** → 必须离线 `astc_retier` / `shrink`。
2. UnityPy 写回后**磁盘常变大**；以运行时 RSS 为准。
3. Addressables 内嵌音频：`audio_stream_patch --allow-embedded`；LoadType 改不动标题贴图峰值。
4. 根分区 ext4 **可开 swap**，TF vfat 不行；swap 往往把 OOM 变成更久假死，不作可玩方案。
5. Allwinner mali-fbdev：部分场景切换用 `BD_EGL_CPU_PRESENT` + 插件 present 轮询，勿在核心写死游戏名。
6. 覆盖运行中二进制可用 `--force`；勿远程乱启游戏 `.sh`（除非明确要求）。

## MEM 样点（止损证据）

| 阶段 | RSS | sys_avail |
|------|-----|-----------|
| Main | 246 MB | 617 MB |
| Base | 334 MB | 529 MB |
| 标题加载中 | →784 MB | →72 MB |
| gameBase 后 | →821 MB | →7 MB |
