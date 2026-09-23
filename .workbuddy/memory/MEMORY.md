# Bogodroid-oddmar — 项目长期记忆

> 详细过程看 `docs/ODDMAR.md`；资源瘦身看 `docs/ODDMAR-ASSET-SLIMMING.md`。本文件只留硬性约束、当前状态和止错点。

## 硬性约束

- 掌机：`172.16.6.77`，Oddmar 根目录 `/mnt/mmc/Roms/ports/Oddmar/`，Dropbeak 0.6.4。
- 大文件推送用 `dropbeak-cli push --force --chunk --chunk-size 16m --verify`；推完核对 sha/大小。超时/失败后当残缺文件处理，整文件重推。
- 掌机上只保留一个运行日志 `/mnt/mmc/Roms/ports/Oddmar/log.txt`；大日志先在掌机侧 grep/tail，必要时再拉本地。
- 构建每次显式带 `-DJNIVM_ENABLE_RETURN_NON_ZERO=OFF`。上机默认：Release + `BD_ENABLE_LOG=ON` + TRACE/VERBOSE/IL2CPP_TRACE OFF + strip。
- 容器 `GlES_Dev` 的源码副本不会自动同步；本机改源码后要 `docker cp` 对应文件到 `/workspace/Bogodroid/...` 再构建。`docker cp` 保留 mtime，必要时 `touch`。

## Oddmar 当前按键状态（2026-09-23 晚）

- 旧结论“`guide/select/back = NONE` 可修 ABXY 弹框”已推翻；用户要求撤销：
  - `guide = "ESCAPE"` 保持；
  - `select/back = "BUTTON_SELECT"` 保持；
  - `Start+Select` 仍为即时退出热键（非 1200 ms 长按）。
- 真根因更像 Oddmar/InControl 的 **Android 手柄按钮路径**：ABXY 作为 `BUTTON_A/B/X/Y` 注入时会在 Press-any-key / 关卡触发退出确认。
- 已验证有效方向：把面键映射成 Oddmar legacy InputManager 的键盘语义：

```toml
a = "SPACE"   # 跳 / UI confirm
b = "K"       # 攻击 / UI confirm
x = "K"       # 攻击副本
y = "SPACE"   # 跳副本
```

- 真机确认：Press-any-key 界面按 A/B/X/Y 均不弹退出框；A 进关与跳跃正常；B 攻击正常；UI/菜单中 A/B 可确认，D-pad 切换按钮。
- 未闭环：键盘化后同一键连续触发有状态刷新问题——A 第一次跳、B 第一次攻击后，再按 A/B 不触发；按一次 D-pad 后才能再次触发。A/B 交替也不能恢复。
- 当前已部署实验 loader：键盘化面键 `ACTION_UP` 后补一帧中立手柄 `MotionEvent`，sha256 `d0aaa1de1d989a2524cec1102850574acf25233886c77b8b8167dafb477b02bd`。**尚待真机验证**。

## 日志判据

- ABXY 正常旧路径：A=96、B=97、X=99、Y=100，device=2/source=`0x1000611`。
- 键盘化路径：SPACE=62、K=39，最新版会走 keyboard device=1/source=`0x101`。
- 若看到 `[BD-EXIT] Start+Select exit hotkey` + `[BD-PREFS] saved` + `unityloader exited (0)`，是 loader 自己退出，不是崩溃。
- 日志降噪：默认屏蔽高频 `[BD-FINDCLASS]` 与 `[BD-ANY-MISS]`；需要完整 JNI 噪声时设 `BD_JNI_TRACE=1`。

## 画面阶段字节数

对话框/暗屏 26–27 KB｜过场黑屏 33–34.5 KB｜关卡地图 ≈229 KB｜进入关卡后 ≈100 KB｜标题 277–282 KB｜采集失败 192 B。判阶段优先看字节数序列，别只肉眼读 PNG。

## 仍需处理

1. 验证 sha `d0aaa1de…` 的中立 MotionEvent 刷新是否修复连续 A/B。
2. 若无效，继续查 Unity legacy keyboard state 刷新；不要回到 guide/select/back 方向。
3. 进关卡峰值内存、视频 640×360 降档、`SoundBanks/` 是否死重仍未收尾。
