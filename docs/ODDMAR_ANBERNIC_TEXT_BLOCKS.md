# Oddmar Anbernic 动态文字色块问题记录

更新时间：2026-10-12

## 当前结论

Oddmar 在 Anbernic 掌机（`172.16.5.40`，目录
`/mnt/mmc/Roms/PORTS`）可以启动，视频也可以解码和播放，进程没有
crash，但部分动态文字/UI 会显示为纯色块。

用户确认：在解决 TrimUI 视频播放问题之前的 Bogodroid 版本中，同一台
Anbernic 掌机运行 Oddmar 完全正常。问题因此高度关联于后续为 TrimUI
视频兼容加入或调整的 FFmpeg/视频纹理 GL 路径，而不是 Anbernic 的
基础启动脚本或游戏资源本身。

## 已确认事实

- TrimUI 掌机运行 Oddmar 正常。
- Anbernic 当前版本可以启动 Oddmar。
- Anbernic 当前版本视频解码成功，日志能看到 `codec configured`、
  `frame sink ready` 和持续的 `swap`。
- Anbernic 当前版本没有观察到 crash；可正常退出。
- EGL/Mali 初始化和 640x480 drawable 建立成功。
- Oddmar 的三个插件均能加载。
- 当前异常表现为部分动态文字变成纯色块，不是全屏黑屏。

## 本轮测试

诊断构建：

- `CMAKE_BUILD_TYPE=Release`
- `BD_ENABLE_LOG=ON`
- `BD_ENABLE_TRACE=OFF`
- `BD_ENABLE_VERBOSE=OFF`
- `IL2CPP_TRACE=OFF`
- `JNIVM_ENABLE_RETURN_NON_ZERO=OFF`

设备使用脚本：

- `/mnt/mmc/Roms/PORTS/奥德玛.sh`

诊断日志：

- `/mnt/mmc/Roms/PORTS/Oddmar/log.txt`

本轮在 `thunks/khronos/gles2.cpp` 中尝试了两项修复：

1. 将 `GL_TEXTURE_2D` 到视频 backing texture 的替换限制为最近一次
   `GL_TEXTURE_EXTERNAL_OES` 绑定的同名纹理，避免任意历史视频纹理名误伤
   普通 UI/字体纹理。
2. 为 shader/program 删除补充状态清理，避免 OpenGL 对象 ID 复用后残留
   `g_rewritten_shaders`、`g_video_programs` 和 shader-program 映射。

重新构建并部署后，Anbernic 仍出现相同的动态文字纯色块问题。因此这两项
修复不足以解决问题，不能视为最终修复。

## 重要日志线索

日志显示视频相关程序能够链接，且使用 `sampler2D`：

```text
[BD-VIDEO] program ... samplers: 2D=... external=0
[BD-VIDEO] swap: sinks=1 ... uploaded=... frames=...
```

这说明当前问题不再是“FFmpeg 没加载”或“视频完全无法上传”。同时，视频
路径仍会在视频播放期间持续执行 backing texture rebind，因此普通 UI 绘制
与视频 GL 状态之间仍存在待验证的相互影响。

## 暂停状态

截至本记录，暂不继续构建、部署或主动启动掌机。

下一步恢复工作时，优先使用“TrimUI 视频问题修复前、且 Anbernic 已确认
正常”的 unityloader 与插件作为二分基线，逐项比较/回退 TrimUI 视频相关
改动。不要先继续堆叠新的纹理绑定补丁；应先确定造成 Anbernic 文字色块
的最小变更范围。

## 2026-10-13：提交整理与验证

BrickGame 已快进合入 oddmar，基线为 `484072d`。本次提交保存上述视频纹理
绑定限制和删除清理尝试，不将其表述为 Anbernic 文字色块的最终修复。
编译检查发现 `bd_glDeleteProgram` 引用的 sampler 缓存定义在后方，已将
该函数移至缓存定义之后，不改变补丁行为。

GlES_Dev 容器中使用当前 unityloader 的实际编译命令，完成修改后
`gles2.cpp` 的 Release ARM64 对象编译；LOG/TRACE/VERBOSE/IL2CPP_TRACE
全部 OFF，`JNIVM_ENABLE_RETURN_NON_ZERO=OFF`。启动模板 `bash -n` 通过，
其默认路径修正为 `/mnt/SDCARD/Data/ports`，游戏资源子目录可通过
`GAME_DATA_DIR` 设置。需要 PortMaster 初始化的游戏继续沿用各自包装脚本。

本次未链接或部署新的 loader，也未再次运行掌机游戏；文字色块问题仍未解决。
