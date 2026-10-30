# 系统音量：sysfs 音量源 + 共享混音器软件增益

适用：固件把音量写成 sysfs 数值、而音频 PCM 满量程的掌机（Anbernic H700 一类）。其它机型走 `auto` 自动退让。

## 为什么需要这一层

这类固件**不是** Pulse，也不是靠旋钮改 ALSA mixer：

- `/etc/asound.conf` 的 `pcm.!default` 用 `ctl_elems` **每次 open 把 `digital volume` 锁成 63**；
- dmenu 系统音量写在 `/sys/class/power_supply/axp2202-battery/openbor_volume`（0–10），由 `volumeCtrl.dge`（tinymix）维护；
- 游戏 PCM 满幅度。不做软件增益，系统音量对游戏**无效**；游戏跑起来后音量键被 SDL 吃掉，CFW 守护进程也不在。

## 实现

| 文件 | 作用 |
|---|---|
| `platform/common/sys_volume.cpp` | 后端选择 `auto`/`sysfs`/`software`/`passthrough`；探测 `openbor_volume`（含 `axp20x-battery` 变体）；对外给 `bd_sys_volume_percent()` / `owns_keys()` / `adjust()` |
| `platform/common/audio_bus.cpp` | **单一 SDL 输出设备 + 软件混音**：FMOD 与 Wwise 各一路 ring，混合后**只在这里**把系统音量施加一次（S16 饱和到 ±32767），每 16 个回调 poll 一次音量源（≈80 ms） |
| `projects/unityloader/javastubs/fakefmod.cpp` | Unity 内置 FMOD 走 `bd_audio_bus_push(BD_AUDIO_OWNER_FMOD)` |
| `thunks/opensles/opensles.cpp` | Wwise 走 `BD_AUDIO_OWNER_WWISE`；shim 按调用方分发，**只给 Wwise**，FMOD 仍走 fakefmod |
| `platform/common/input_backend.cpp` | 仅当 `bd_sys_volume_owns_keys()` 时吞掉 `VOLUMEUP`/`DOWN`（含 KEYUP），改调 `bd_sys_volume_adjust()` |
| 启动脚本 | 无 Pulse 时 `SDL_AUDIODRIVER=alsa`；**仅当** `/proc/asound/cards` 有 `audiocodec` 才设 `AUDIODEV=plughw:audiocodec` |

## 配置

```toml
[audio]
backend = "auto"          # auto / sysfs / software / passthrough
# sysfs_path = "/sys/class/power_supply/axp2202-battery/openbor_volume"
# sysfs_max = 10
# intercept_volume_keys = true   # 强制拦截；auto 下按后端决定
```

环境变量优先于 toml：`BD_SYS_VOLUME_BACKEND`、`BD_SYS_VOLUME_PATH`、`BD_SYS_VOLUME_MAX`。

## 已知限制（换机必读）

- 线性 `sample * pct / 100`，不是 dB。Anbernic 0–10 与系统观感已对齐，别的机子若用对数 mixer 可能听感不同。
- `AudioManager.getStreamVolume` 仍是 stub 返回 100：游戏内音量滑块不反映真实响度，改音量只能靠系统键。
- 音量键若被映射成手柄键而不是 `SDL_SCANCODE_VOLUMEUP/DOWN`，不会被拦截。
- 没有 ALSA mixer 后端：mixer 被 `asound.conf` 锁死、音量又写在未知路径时，必须填 `sysfs_path`，否则保持 100% PCM。
- `auto` 以 `SDL_AUDIODRIVER` 是否为 `pulse`/`pipewire` 判断 Pulse，避免残留 Pulse 套接字把 sysfs 盖掉。强制 `intercept_volume_keys=true` 叠在 Pulse 上会双重衰减。
- 混音器是「一路 FMOD + 一路 Wwise」的加法饱和，两路同时满幅会削顶；不要指望它做限幅。

## 状态

Anbernic 真机验证有效（Hollow Knight 现场）；TrimUI 及其它机型未逐项验证。

> 来源：原 `ANBERNIC.md` §3.6。2026-10-29 按 `sys_volume.cpp` / `audio_bus.cpp` / `opensles.cpp` 重写——
> 旧文说「增益只作用在 fakefmod」「开 `BD_ENABLE_OPENSLES_SHIM` 会让 FMOD 静音」，
> 那是共享混音器改造**之前**的行为，现已不成立。
