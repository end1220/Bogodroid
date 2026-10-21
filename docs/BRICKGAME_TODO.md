# BrickGame 待办

更新日期：2026-10-21

## Unity 6 集成

- [ ] 评估并整合 `unity6` 分支到 `BrickGame`。此前只完成可行性分析和
  `git merge-tree` 模拟，没有执行合并。两分支共同祖先为 `b932d82`；
  Unity6 有 9 个独有提交，BrickGame 有 41 个独有提交；模拟合并发现
  14 个文本冲突，主要在 JNI 桩、Unity 启动流程和 EGL。
- [ ] 在独立集成分支逐项解决冲突，保留 Unity 旧版本启动回退，以及
  BrickGame/Oddmar 的视频、splash、待机和插件 ABI 行为。
- [ ] 容器验证 Unity 6000.x 测试程序；回归 BrickGame、Oddmar；最后在掌机
  验证 Unity6 画面、手柄和正常退出。Unity6 分支已有容器和 H700 真机验证，
  但这不能替代合入后的回归。
- [ ] 按项目构建说明显式设置
  `-DJNIVM_ENABLE_RETURN_NON_ZERO=OFF`，并使用单独构建目录。

## TrimUI 1280×720 显示与左方向键

- [ ] 获取问题机型、固件版本、BrickGame 启动日志和现场画面，确认 SDL
  display mode、SDL window/drawable、EGL surface 与实际面板是否都为
  1280×720。当前自动探测优先 SDL、其次 `/dev/fb0`；错误尺寸会导致 Unity
  逻辑分辨率与 drawable 不一致。特别检查默认 framebuffer `glViewport` 是否
  与 drawable 尺寸相符，以及设备是否沿用 640×480 配置。
- [ ] 读取启动日志中的 `[BD-DEVICE]`、`[BD-EGL_SDL]`、`[BD-VIDEO]` 和
  `SDL_NumJoysticks`/GUID；现有 BrickGame 日志在启动脚本中重定向到
  `/dev/null`，诊断构建/启动时需临时保留日志。
- [ ] 按左键时确认 SDL 是否收到 `SDL_CONTROLLERBUTTON_*` 的
  `DPAD_LEFT`、`SDL_JOYHATMOTION` 或 `SDL_KEYDOWN/UP`。现有 BrickGame 配置
  使用 `controller_key_events=false`，游戏移动由插件把 SDL 控制器方向键
  转成屏幕触摸；因此若该设备的左键没有被 SDL 识别为 `DPAD_LEFT`，或映射到
  其他按钮，Android `KEYCODE_DPAD_LEFT` 路径不会补救插件触摸路径。
- [ ] 若确认是设备 GUID 缺少/错误映射，为该机型加入 SDL GameController
  mapping（优先放入设备 `gamecontrollerdb.txt` 并由启动器加载）；若固件只产生
  joystick hat/key 事件，再补相应 SDL 事件兼容。保持其他键映射不变并容器回放
  左、右及斜向按键后上机确认。
- [ ] 显示修复应以实际 1280×720 drawable 为准，避免为适配单台设备直接硬编码
  BrickGame 的 Unity 逻辑分辨率。若发现视口/裁切问题，再按日志区分面板探测、
  framebuffer mode 与 Unity viewport，不先改游戏 Canvas 布局。

### 当前排查结论

源码中没有 TrimUI 1280×720 设备的现场 SDL GUID、按键事件或显示日志，因此
尚不能唯一判定根因。已有证据支持两条重点假设：

1. **左键输入**：BrickGame 用插件屏幕触摸而非通用 Android 按键注入；按键是否
   有效取决于 SDL GameController mapping 与插件收到的逻辑 D-pad 事件。其余方向
   键正常使“仅左键映射异常”成为优先检查项；缺少该设备的事件日志前，不能确认
   是键位 mapping、SDL hat 报告还是固件问题。
2. **显示**：配置将 `displayWidth/Height` 留为自动探测，启动器没有按机型覆盖。
   设备报告 1280×720 时预计会请求该窗口/GL drawable 尺寸，但 SDL/fb0 数据与真实
   面板若不一致，Unity viewport 会出现裁切或留边。项目另有视频在 1280×720
   viewport 与较小 drawable 不匹配导致左下区域裁切的历史案例；这说明值得检查
   尺寸链路，不能据此认定本次 BrickGame 已发生同一问题。

需要从该 TrimUI 取得信息：准确型号/固件、屏幕异常的具体表现或照片，以及能保留
上述 SDL/BD 日志的一次启动记录。拿到日志后可区分显示尺寸链路和 controller GUID
问题，再做有针对性的修复。
