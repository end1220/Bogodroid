# BrickGame 待办

更新日期：2026-10-23

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

## TrimUI 1280×720 显示与左方向键（已完成）

- [x] 保持屏幕尺寸自动探测，边框横向铺满；女孩装饰独立保持原比例并根据游戏区域定位。
- [x] 将插件固定点击坐标改为按钮 RectTransform 实际中心，适配不同宽高比。
- [x] 容器 1280×720 回放 A、左、右键，确认触摸按下和松开正常。
- [x] 用户在 Anbernic 和 TrimUI 真机测试通过，并更新桌面两个平台包。

### 修复与验证

固定 640×480 按钮坐标在宽屏布局下偏离实际按钮中心。新逻辑通过
`Transform.TransformPoint` 取得按钮矩形中心的世界坐标；Overlay Canvas
直接使用其屏幕像素位置，Camera Canvas 使用 `Camera.WorldToScreenPoint`。
最后转换为触摸注入接口的参考坐标，并将 Y 轴从左下原点转为左上原点。
按住和松开沿用按下时的位置，节点未就绪时不点击。

首版运行时投影曾依赖该游戏未保留的 `RectTransformUtility.WorldToScreenPoint`，
导致所有点击被跳过；容器诊断确认方法缺失后，已移除此依赖。
1280×720 容器中左键的参考坐标为 `(246.9,375.0)`，旧值为 `(222,375)`。
用户于 2026-10-23 确认 Anbernic 与 TrimUI 测试通过。

本次修改仅在 `brickgamepro` 插件内。正式包 loader 保持 Release、
LOG/TRACE/VERBOSE/IL2CPP_TRACE 全 OFF；桌面包已同步验证过的插件与
项目 `gamecontrollerdb.txt`。设备键位数据库仍由 loader 加载，不由插件覆盖。
