# H700 视频硬解探测（2026-09-17）

## 结论

当前 Bogodroid/FiveHearts **不接 H700 VPU**。P1 已采用 FFmpeg 软解 +
I420 三平面上传 + GLES YUV→RGB：854×480 实测约 23–28fps，已足以继续验证游戏。

硬件具备 Cedar 视频引擎，但当前 64 位运行环境没有一条可直接复用的用户态解码链：

- 有 `/dev/cedar_dev` 和 `/dev/ion`；
- 内核启用 `CONFIG_VIDEO_ENCODER_DECODER_SUNXI=y`，但
  `CONFIG_V4L_MEM2MEM_DRIVERS` 未启用；
- `/sys/class/video4linux` 只有 CSI/ISP/scaler 子设备，没有 decoder M2M 节点；
- 系统 `/usr/bin/ffmpeg` 只列出 `drm` hwaccel，H.264 只有普通软件 decoder；
- `/usr/bin/mpv --hwdec=help` 没列出可用的具体硬解后端；
- 供应商目录有 32 位 ARM `xplayerdemo2`，其 ELF 依赖 CedarX
  `libvdecoder.so`、`libVE.so`、`libMemAdapter.so` 等，但当前系统路径未找到这些库；
- Ports 实际视频入口 `FFplay.sh` 调用 32 位 bundled `ffplay`，显式
  `-vf scale=640:-2`；二进制没有 Cedar/vdecoder 依赖。它的流畅不能作为 VPU 已打通的证据。

因此当前判定为 **No-Go（直接接入）**：不能从 64 位 unityloader 使用缺失的 32 位
CedarX ABI，也没有 V4L2 request/M2M 可接 FFmpeg。

## 若以后重开

满足以下任一前置条件再继续：

1. 获得与 H700 固件匹配的 **aarch64 CedarX** 头文件和运行库；
2. 内核/FFmpeg 补齐 stateless V4L2 request 或 V4L2 M2M H.264 decoder；
3. 独立 32 位解码进程可稳定输出 DMA-BUF，并定义跨进程 fd/同步协议。

Go 阶段还必须验证：

- 解码输出为 NV12/I420 DMA-BUF，时间戳和 seek 正确；
- `EGL_EXT_image_dma_buf_import` 可把它导入 Mali；
- fence/缓存一致性正确，连续播放无撕裂、泄漏和句柄增长；
- 相比当前 P1 的 CPU、功耗和帧率收益足以覆盖维护一套专有 ABI 的成本。

在这些条件出现前，推荐路线是 P1 GPU 色转 + APK 构建前离线转成
854×480@30、低参考帧 H.264。
