# BrickGame 与 oddmar 合并评估

评估日期：2026-10-20。以下保留合并前分析；实际执行结果见末尾。

## 分支关系

- 共同祖先：`484072d40dbd1ce263160cb540602265586e4ae2`。
- 本次整理前 BrickGame：`9ba5716`，相对祖先独有 4 个提交。
- oddmar：`68d0abc`，相对祖先独有 1 个提交。
- oddmar 以前已包含共同祖先上的 BrickGame 工作，但不包含此后的 splash、
  新版输入、速度及待机改动。两条分支已分叉，不能直接快进。

| 分支 | 独有内容 | 影响范围 |
| --- | --- | --- |
| BrickGame | splash 视频与启动时序、首帧通知、插件 API 尾部扩展 | 通用 loader / EGL 与 BrickGame 插件 |
| BrickGame | 方向／旋转、Guide／Start、字体与 UI、美术工具、速度调整 | BrickGame 插件及资源工具 |
| BrickGame | 电源键拦截、帧边界暂停与内核待机 | 所有使用该 loader 的 Unity 游戏 |
| oddmar | 视频纹理重绑定限制、shader/program 删除时清理缓存 | 通用 GLES 包装 |
| oddmar | Anbernic 文字色块调查、TrimUI 启动脚本模板 | 文档及独立模板 |

## 合并可行性

`git merge-tree --write-tree HEAD origin/oddmar` 返回成功，无文本冲突。
本次整理前合并树为 `9718dc40a9eb96afedc1b998ff11ebce606eed74`；
本文档和发布摘要整理后树 ID 会变化。该检查不会修改当前索引、工作区或分支。

oddmar 独有提交只涉及以下三个文件，BrickGame 分叉后的提交未修改它们：

- `thunks/khronos/gles2.cpp`
- `docs/ODDMAR_ANBERNIC_TEXT_BLOCKS.md`
- `scripts/trimui_unity_launcher_template.sh`

因此建议保留两边历史，普通 merge 即可，无需 rebase 或逐个 cherry-pick。
oddmar 的 GL 补丁限制 2D 视频替换到最近的 external texture，并清理删除的
shader/program 记录；它未解决 Anbernic 文字色块，不能当作已验证的最终修复。

## 推荐顺序

1. 在 BrickGame 上合入 `origin/oddmar`，作为集成验证分支。
2. 使用独立构建目录编译 loader 和全部部署所需插件，显式设置
   `-DJNIVM_ENABLE_RETURN_NON_ZERO=OFF`。正式构建保留 Release、strip 和低日志；
   排查时按仓库约定打开基础 LOG，TRACE/VERBOSE/IL2CPP_TRACE 保持 OFF。
3. 验证以下回归，再将集成结果合回 oddmar。测试通过后的 oddmar 可以快进到
   集成提交，前提是期间没有新提交。

```sh
git switch BrickGame
git merge --no-ff origin/oddmar
# 构建、回归及审查完成后推送集成分支，再在 oddmar 工作区执行：
git merge --ff-only origin/BrickGame
```

oddmar 已在 `D:/Locke/gitee/Bogodroid-oddmar` 工作区检出，不能在本工作区强行
切到该分支；应先检查那个工作区是否干净、是否存在进行中的任务。上述命令是
后续操作建议，本次没有执行，也没有修改另一工作区。

## 合并后的运行验证重点

- BrickGame：640x480 与 720x720；splash 最少三秒、原始开场动画、场景交接；
  游戏内 Guide 返回列表、列表 Guide 打开退出框、Start 暂停／继续、A/B 确认／取消；
  Tank 和 G 的速度、字体与方块图案。
- 待机：电源键路径在公共 loader 中无游戏限制。Oddmar 也会拦截电源键并写入
  `/sys/power/state`，因此要在各固件验证真实熄屏、唤醒、音频与游戏更新恢复，
  以及打开／写入 sysfs 失败后的恢复。容器模拟不能替代物理测试。
- Oddmar：TrimUI 的视频解码、纹理切换和结束后普通 UI；Anbernic 的动态文字
  色块应与已知基线对照，不能因无合并冲突便声称修复。
- 插件兼容：新增 startup 字段位于 v8 API 尾部，未移动旧字段；旧插件行为仍需
  用实际 loader 验证，新 BrickGame 插件应与新 loader 配套发布。
- 新启动模板是独立文件，不应直接覆盖各平台已验证的正式脚本。Oddmar 的
  FFmpeg 搜索路径与 PortMaster 初始化继续沿用原有配置。

## 本次整理和验证

当前发布摘要已更新为 2026-10-20 的真实端点、行为与文件哈希；历史验证保留。
插件 API 的视频说明已补充三秒最低播放和 nativeResume 时序，未更改接口布局。
只变更文档和注释，不改变掌机二进制。已检查差异空白、文档相对链接及 Git
模拟合并结果。本次未构建合并后的二进制、未部署、未声称完成跨游戏回归。


## 已执行合并与分支清理（2026-10-20）

用户随后明确授权合并及删除已合入分支。合并提交 `d07ac5d` 将 oddmar
合入 BrickGame，无文本冲突，并已推送 origin/BrickGame。

删除前已确认 origin/ddmar、origin/skul、origin/video、origin/anbernic
以及存在的同名本地分支全部是合并后 HEAD 的祖先，没有独有提交。
已删除这四个远端分支及本地 skul、video、anbernic（本地原本没有 ddmar）。
oddmar 分支保留。

video 原工作区 D:/Locke/gitee/Bogodroid-anbernic 已切至原提交 058ddca
的 detached HEAD；目录和未跟踪的 .workbuddy/ 保留，未删除工作区。

合并后的 GLES 源文件通过 Release ARM64 对象编译，启动模板通过 bash -n，
差异空白检查通过。本次未链接、部署完整 loader，未进行跨游戏运行回归；
掌机和桌面发行目录仍使用此前已验证的发布版本。
