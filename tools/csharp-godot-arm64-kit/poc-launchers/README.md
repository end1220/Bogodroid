# POC launcher presets

godot 4.5 + SDL2 POC 真机测试的 launcher 套件。**一份 godot binary 通过这 21 个 preset 测 21 种配置**,不用重编。

## 部署

```bash
# 1. 设备:godot binary 已就位
ls /mnt/SDCARD/Data/ports/poc_sdl2/godot

# 2. 推 shim_egl.so(诊断 hook,见 ../shim_egl/)
scp ../shim_egl/shim_egl.so root@device:/mnt/SDCARD/Data/ports/poc_sdl2/

# 3. 推所有 launcher 到 PORTS/
scp '[POC]'*.sh root@device:/mnt/sdcard/mmcblk1p1/Roms/PORTS/

# 4. 清 PortMaster cache,新 launcher 刷出
ssh root@device 'rm /mnt/sdcard/mmcblk1p1/Roms/PORTS/PORTS_cache*.db'
```

## Preset 分类

### 01-12:`POC_*` env config toggle(从 binary 内部 toggle 行为)
| | env 设置 | 用途 |
|---|---|---|
| 01-default | (无)| baseline |
| 02-no_master | POC_DRM_MASTER=0 | 测 Mali 是否要自抢 master |
| 03-argb_a8 | POC_GBM_FORMAT=ARGB + POC_EGL_ALPHA=8 | format 兼容 |
| 04-gbm_linear | POC_GBM_LINEAR=1 | 关 modifier |
| 05-no_plat_ext | POC_EGL_PLATFORM_EXT=0 | eglGetDisplay 而非 EXT |
| 06-no_depth | POC_EGL_DEPTH=0 | 关 depth buffer |
| 07-gles2 | POC_EGL_GLES_VER=2 | 老 ES 2.0 context |
| 08-skip_pageflip | POC_SKIP_PAGE_FLIP=1 | 不 page-flip |
| 09-skip_makecurrent | POC_SKIP_MAKE_CURRENT=1 | 不 eglMakeCurrent |
| 10-skip_surface | POC_SKIP_EGL_SURFACE=1 | 不创 surface |
| 11-skip_egl | POC_SKIP_EGL=1 | 跳整段 EGL |
| 12-skip_kms | POC_SKIP_KMS=1 | 跳整段 KMS |

### 20-28:LD_PRELOAD shim_egl.so blacklist(从外部 hook GL 探测)
| | EGL_SHIM_BLACKLIST | 用途 |
|---|---|---|
| 20-shim | (空) | 只 hook 看 log,不 ban,看 SIGSEGV 前最后查哪个 |
| 21-block_compute | compute/memory_barrier 系列 | ban GLES 3.1 compute |
| 22-block_multiview | OVR multiview 系列 | ban OVR 扩展 |
| 23-block_khr_debug | debug message/object label 全套 | ban KHR_debug |
| 24-block_storage3d_multi | TexStorage2/3DMultisample | ban 多采样存储 |
| 25-block_aggressive | 上面 4 类合集 | 一次 ban 常见 trouble |
| 26-shim_full_strace | (无 blacklist + 全 strace)| 最详细 forensic |
| 27-block_31_compute | 显式 3.1 compute/indirect/multisample | 保守版 |
| 28-block_all_31_32 | 全 78 个 3.1+ 函数 | 最激进 |

## 输出文件(每个 preset 写四个文件)

```
$GAMEDIR/<preset>.log      tee 后的 stdout/stderr 全
$GAMEDIR/<preset>.diag     poc_diag 写的 syscall-safe 诊断
$GAMEDIR/<preset>.shim     shim_egl.so 写的 hook log
$GAMEDIR/<preset>.strace   strace 写的 syscall log
```

bisect 流程:
1. 跑 `[POC]20-shim` → `cat <preset>.shim | tail -20` 看 SIGSEGV 前最后查询
2. 把那函数名加 `EGL_SHIM_BLACKLIST` 跑
3. 死另一个继续加,直到 godot 启动完(应看到 `[POC-DIAG] DSDL2: CONSTRUCTOR COMPLETE` 之后 godot 自己的 `Godot Engine v4.5...` 横幅)
