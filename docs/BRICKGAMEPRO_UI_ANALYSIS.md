# BrickGamePro：界面与美术适配

## 2026-10-16：退出确认图片与 Yes/No 虚线框（已验收）

新 `exit-game.png` 为 571×464，白底改透明，灰边为黑色半透；完整分辨率
PNG 嵌入插件，显示尺寸约 540×438.8，中心 (0,260)，不接收点击。
原文字、按钮及 FrameButton 统一下移 260，按节点记录防止重复位移。
背景 Image 位于 ExitMenu 本身。FrameButton 下的 Slash、Slash (1..4)
才是 Yes/No 虚线框与中线，必须随按钮移动；之前误将其当背景而留下，
导致框套在标题位置，现已修正。图片宽高比也独立于 Game Over 图片，
避免纵向压缩。A 确认退出、B 取消及按键松开消费逻辑不变。

Release、全部日志 OFF、显式 RETURN_NON_ZERO=OFF，strip 后插件
`fecb2099c79a26cf9af30a08f20cfae35e22e195aa940c9facfc74feb0638022`。
容器与 Trimui 真机截图确认图、标题、按钮无重叠，虚线框包围 Yes/No；
Guide → B → Guide → A 正常退出，MainUI 恢复，无 `[BD-*]` 行。
两台掌机各 145 项运行文件核验通过，远端临时诊断文件已清理。
用户明确确认修改通过。证据为本地 ignored
`deploy/BrickGamePro/exit-border-validation/`。本节哈希是整理代码前的已验收
部署产物；之后构建产物可变。后文为历史验证记录。

## 2026-10-15：修正 Boy 两帧对齐

用户反馈两张已对齐的 512×512 源图在切换后出现轻微位移。确认是替换工具的
布局错误：先分别裁 alpha 范围，再各自缩放和居中。Boy1 为 363×363→152×152，
Boy2 为 365×372→155×158；同一源坐标 (256,200) 分别落在逻辑画布约
(127.209,111.529) 和 (129.233,110.161)，偏差约 (2.024,-1.368)。沿用前一版
per-sprite manifest 只延续了这一错误，不能保证源图坐标对齐。

工具现删除独立裁切/缩放/居中流程。两张完整源画布统一 512×512→215×215，
共用逻辑 256×256 画布位置 (20,30)。根据原 textureRectOffset 计算两个 packed
rectangle 在逻辑画布的位置：(34,53,221,209) / (33,47,215,209)，分别裁取后
回写；UV 和 Sprite 数据不变。每张写回后都按原偏移还原，与统一变换画布
逐字节比较，无裁切或位置误差。`--layout-manifest` 现在只接受包含共享变换
的新记录，拒绝旧独立变换；`--boys-only` 可单独处理小人，不触碰 Game Over。

新资源仍只修改 split7，SHA-256：
`a6e239e57c8935b2db84a07f98bb162942ab2a054e2e1d97918c05e9091f27fe`。
16991 个其他对象及未涉及的压缩块不变，未改变透明度。容器 finish 回放通过，
A 返回和重新开局正常。输出保存于桌面 `BrickGame/替换/对齐修正-20261015/`，
含两张逻辑画布、ETC2 回读画布、修正前后两帧切换 GIF 和原因说明。
证据：`deploy/BrickGamePro/boy-alignment-validation/`。安装包已更新，SHA-256：
`3f54a90da3e6ecbca5625821ac6d2df2dfbe2a0cc1f6871c67cd425145ac4072`。
首次连接 `172.16.7.34:8080` 超时；用户随后要求推送并清理掌机目录，现已完成。
仅更新 split7 和 SHA256SUMS，插件仍为上版单图放大版本。激活前确认游戏退出，
上传 `.new` 后验证新旧哈希；清理前后全部 145 个运行文件均通过 SHA-256 校验。
按用户要求删除 14 个 backup 目录、5 个 `.bak` 文件及 8 个旧日志/校验文件，
共 53 个文件约 50.23 MiB；磁盘占用从 117984 KiB 降至 65408 KiB。
保留 conf、cache、存档、配置及所有运行文件，两个存档文件清理前后哈希一致。
原 log.txt 已保存至本地 `boy-alignment-validation/device-before-cleanup.log`。
目录现无 backup、.bak、.new、.tmp 或旧校验残留；清单、前后目录快照和部署
记录保存于 `boy-alignment-validation/`，本轮未远程启动游戏。

## 2026-10-15：手工线稿更新与单图放大

再次替换桌面 `替换/` 中用户手工修改的三个 RGBA PNG。本次使用
`replace-boy-art.py --preserve-alpha --layout-manifest <上版 manifest.json>`，
不再执行白底、颜色或透明度转换，输出副本与输入文件字节完全一致。
Boy1/Boy2 复用上版 source crop、缩放尺寸和矩形内放置位置，以免用户删改
轮廓边缘后触发自动重排；LANCZOS 缩小至 152×152 / 155×158，UV 和 Sprite
对象不变。只修改 split7；16991 个其他对象及图集未涉及块保持一致。

Game Over 使用原尺寸 651×384 PNG 原样嵌入，无离线缩图。显示尺寸较上一版
增大 15%，从 270.0×159.3 调整为 310.6×183.2 逻辑单位，中心 y 从 144 下移
至 136，x=0 保持居中。640×480 时显示矩形约从 108×64 增至 124×73 像素。
容器 finish 回放通过：单图正确创建、10 次隐藏状态检查正常、A 返回后重开
正常，无原生崩溃。截图确认新线稿完整，单图更大且下移，无白底或裁切。

本地结果：桌面 `BrickGame/替换/替换结果-20261015-v2/`，含新图集、ETC2
回读、输入副本、上一版资源备份和前后对比截图。验证证据：
`deploy/BrickGamePro/boy-single-icon-v2-validation/`。
插件 SHA-256：`479acaed2280a2e96b77d5e72155afe39fb1c4e7d6f9db46abd39d1e1d8150cf`；
split7：`f75911d5397faff704a6444ae765eac7bda9657e828059393dbc7aea6a5f90cc`。
本地包已更新，145 文件、57,927,680 字节，SHA-256：
`ea496fdbece1c5b9cb689d072bedf5af3faf6a28dda453b55206c229ec2e02e7`。

用户确认掌机上线后已推送至 `172.16.7.34`。激活前确认游戏已停止，旧插件、
split7 和清单备份于 `backup-boy-single-icon-v2-20261015-173845/`。全部 145 个
运行文件通过 SHA-256 校验，BlueNew split11/12 保持一致。未远程启动游戏。
部署记录：`boy-single-icon-v2-validation/deploy-anbernic.txt`。

## 2026-10-15：Boy 图集替换与 Game Over 单图

当前版本使用用户 `替换/` 目录的 Boy1、Boy2 和 gameover-icon。三张输入均为
RGB，gameover-icon 的浅色棋盘格实际烘焙在像素里。统一转为纯黑 RGB，alpha 为
`clamp(1 - luminance/235, 0, 1)`，清除白底和近白棋盘格，保留灰色边缘的半透明
覆盖率。处理后的原尺寸 RGBA PNG、新图集和 ETC2 回读图保存于桌面
`BrickGame/替换/处理后-20261015/`，原始输入未覆盖。

专用工具：`projects/unityloader/plugins/brickgamepro/tools/replace-boy-art.py`。
以当前 staging 为底保留最新 BlueNew，Boy 去透明边距后等比 LANCZOS 缩小，
在原 packed rectangle 内留 2 像素安全边距；Boy1 为 152×152，Boy2 为 155×158。
保留 Sprite 的浮点 textureRect、UV、网格、offset 和逻辑尺寸；只重编码两个矩形
触及的 ETC2 4×4 块，图集其余像素回读逐像素一致，16991 个其他序列化对象
逐字节一致。仍为 2048×1024 ETC2_RGBA8、无 mipmap；仅 split7 改变，分片长度
不变。split11/12 的 BlueNew 哈希保持不变。旧 split7 和旧插件随本地产物备份。

插件删除四图 UV、间距和排布逻辑，删除旧 meme.png，改嵌入透明 gameover-icon。
只创建一个 RawImage，FinishMenu 居中 x=0、原纵向中心 y=144，尺寸 270.0×159.3
逻辑单位。高度与前一版 Share/Best 图标相同，651:384 比例不变，无交互。
六个原动作继续隐藏，A 返回原菜单流程不变。临时 PNG 数组和纹理 GC handle
及时释放，纹理丢弃 CPU 副本后由 RawImage 持有。

容器 Release、LOG=ON，其余 trace/verbose 关闭，RETURN_NON_ZERO=OFF，strip。
finish 回放通过：一个图标创建、x=0、19 个隐藏 renderer alpha=0、六动作 inactive，
A 返回和再次开局正常。640×480 截图确认小人显示完整、无原网格轮廓裁切，
Game Over 单图居中且无白底/棋盘格。证据保存于
`deploy/BrickGamePro/boy-single-icon-validation/`。后续四图章节为历史记录。

finish/buttons/exit 三组最终校验均通过：21 次 UI 采样，一张装饰图，A 返回后
重开正常，退出框 B 取消、A Quit exit=0；0023/0052 帧分别确认两种小人姿态。
首次 exit 回放在第二次退出框操作前出现 Android SIGSEGV，栈顶位于容器的
`ld-2.31.so`，尚不能确定根因，未算作通过。日志保留在 `exit-first/`；相同
二进制和资源复测 exit 通过。校验器增加 Android crash 信号识别，避免把
原生异常后的 exit=0 当成正常退出；没有修改或放宽退出流程断言。

本地最终插件 SHA-256：
`42896d3addc567ff342fb7f844873eda979766f98180bd598be626c9b429e084`。
新 split7：`bbf0e40d6df7646fdc9e644d6b4b1be2565e9db69272008dcdd2ff6da682ca53`。
本地安装包已更新（145 个运行文件、57,937,920 字节），SHA-256：
`1a1bcdbb6adde208c6b3967b585e9654cf3bb7e3a3b493fea3e3d5b08339e364`。
随后按用户指示推送到 `172.16.7.34:/mnt/mmc/Roms/PORTS/BrickGamePro`。
激活前确认游戏已退出，新插件、split7 与清单均先上传 `.new` 并验证 SHA-256。
旧文件保存于 `backup-boy-single-icon-20261015-171113/`，最终 145 个运行文件
全部校验通过，BlueNew 的 split11/12 哈希保持一致。未远程启动游戏；本地
部署记录为 `boy-single-icon-validation/deploy-anbernic.txt`。

## 2026-10-15：Game Over 四个装饰图标

此前将用户的 `memeGray.png` 原样替换插件 `assets/meme.png`。新图仍为
540×360，RGB 三通道相等，透明通道与彩色版逐像素一致，因此沿用已经校准的
UV、尺寸及间距。容器 finish 回放确认四图显示为灰度，A 返回及重新开局通过。
证据：`gameover-icons-gray-validation/`。已推送至 172.16.7.34，145 个运行文件
SHA-256 校验通过；旧插件和清单位于 `backup-gameover-icons-gray-20261015/`。
灰度版插件 SHA-256：`6dc9e7db4e7deea4ec9a3422c2bc86d119608072946b2acc2d4842e6c9e77b93`。

UV 校准版按原 PNG 的实际像素范围逐图指定 UV，不再按四等分单元的中心排布。
以左上角为原点的 `(x,y,w,h)` 为 `(28,5,184,168)`、`(313,4,191,168)`、
`(22,180,200,172)`、`(313,180,207,171)`，覆盖 alpha >16 的全部像素并保留
两像素边距。保持放大版的源像素比例，按裁切边界留 24 逻辑单位水平间距，
整排宽 813.5，在宽 902 的 FinishMenu 内居中，纵向中心不变。这样中间左图
向左、中间右图向右分开，四图可见内容的间距一致，人物没有再次放大或拉伸。
容器 finish 回放验证四图创建成功，原按钮仍隐藏，A 返回及重新开局通过；
640×480 画面人工检查无重叠和裁切。证据：`gameover-icons-uv-validation/`。
该版插件 SHA-256：`c6d33f2d805204b7819ce84ec35f91a67f2f007ed2b75b8a64bc3e7d28dbca52`。
已推送至 172.16.7.34，激活前确认游戏未运行，145 个运行文件 SHA-256 校验
通过。旧插件和清单保存于 `backup-gameover-icons-uv-20261015/`，未远程启动游戏。

此前按用户反馈将图标宽高分别扩大至初版的 2 倍（256×170.7 逻辑单位），
保留原中心间距。四个图标创建完成后，用 FinishMenu 的实际 Rect 和整排图标
左右边界计算居中偏移，整体左移 84 逻辑单位；整排宽 798，面板宽 902。
纵向中心不变。后续验证证据：`gameover-icons-centered-validation/`。
该版 finish 回放与 A 返回后重新开局通过，已推送 172.16.7.34，145 个运行文件
校验通过；上一版备份至 `backup-gameover-icons-centered-20261015/`。
此前居中版插件 SHA-256：`c4bca97d77751ac797babf9d83ce40c5d747e54d16ecd8549f26edf2eb018cbb`。

用户提供的透明 `meme.png`（540×360，2×2）保存在插件 `assets/`，构建时
嵌入 `.so`。插件 `gameover_icons.cpp` 在 Unity 线程上创建四个 RawImage，
按左上、右上、左下、右下分别对应 Share、Best、Menu、Replay。读取原按钮的
RectTransform 锚点、尺寸与 pivot，保持原中心，等比缩放至 128×85.3 逻辑单位。
图标作为 FinishMenu 的直接子节点随菜单显隐，共用一张纹理，无 Button、文字、
点击回调或 raycastTarget；原六个动作仍 inactive，Game Over 的 A 返回不变。
纹理加载后丢弃 CPU 像素副本，四个 RawImage 持有纹理引用；没有逐帧创建或解码。

容器 Release + LOG=ON、其余 trace 关闭、RETURN_NON_ZERO=OFF、strip 构建通过。
finish/buttons/exit 三组回放全部通过：四个图标各创建一次，21 次采样均为
19 个隐藏 renderer alpha=0、六个动作 inactive；A 返回后能重新开始游戏，
退出确认 B 取消、A 退出 exit=0。640×480 Game Over 截图人工检查通过。
证据位于本地 ignored `deploy/BrickGamePro/gameover-icons-validation/`。

已部署至 `172.16.7.34:/mnt/mmc/Roms/PORTS/BrickGamePro`，推前确认无游戏进程，
旧插件及清单备份至 `backup-gameover-icons-20261015-143000/`；全部 145 个运行
文件通过 SHA-256 核对，用户最新 BlueNew 皮肤仍一致。插件 SHA-256：
`ee60adcec765e09316826c34723b8ab37878228fb2d7c694a91f117f2ac75c40`。
未远程启动掌机游戏，实际显示待用户从菜单验证。

## 2026-10-14：启动后按钮无响应的评分分支

现场日志来自用户桌面 `log.txt`，副本保存在 ignored staging
`deploy/BrickGamePro/startup-input-validation/device-original.log`。按键与模拟
触摸 DOWN/UP 均到达，视频资源在 +8913ms 交接前释放；但首个 GUI Update
的 `menuStatus=0`，此前有 `blocked GUIManager.ShowFeedBackMenu`，直到第二次
Guide 关闭退出框才在 ticks=17389 变为 `menuStatus=2`。

原 ARM64 IL2CPP 证据：`IntroductionMenu.Start`（RVA `0x26F3BC`）读取
`SaveManager.playCounts`，等于 5 或 10 时调用 `GUIManager.ShowFeedBackMenu`
（`0x24BA90`）并直接返回，不启动正常 Logo 协程。现有拦截仅隐藏评分框并
return，遗漏评分框关闭后的菜单初始化，留下 `MenuStatus.None=0`。
`PressStartPauseGame`（`0x248DC4`）在 `0x248E00` 判断菜单 1/2，None 不进入
Logo/选游戏分支。原 `level0` 的 `FeedBackMenu/NoBtn` UnityEvent 为
`BackSelectGameMenu`（`0x247F30`）；该方法关闭遮罩并在 `0x248140` 写入菜单 2，
再调用 `IntroductionMenu.ShowCurrentGame`。Guide 关闭退出框也走这条路径，
因此能恢复响应。问题是屏蔽评分框时漏了状态收尾，不是视频线程或按键丢失。

修正属于 BrickGame 插件：ShowFeedBackMenu 拦截设置待处理标志，下一次原
GUIManager.Update 后调用原 No 动作，再隐藏评分 UI，在报告场景就绪之前
完成。不直接写 menuStatus，不改玩家存档计数，不打开商店。

容器通过只调整测试存档的 playCounts 重现：seed=4 的下一次启动触发计数 5，
旧插件停在 menu=0，Start 无法进入，Guide 两次后变为 2。修正版分别以 seed=4
和 seed=9 触发计数 5/10，均出现 `feedback skipped via original No action menu=0->2`，
随后在任何 Guide 操作前已进入俄罗斯方块。两组均经原 Quit exit=0 并保存偏好。
普通 seed=0 路径仍为 menu=1 → 2，不触发评分收尾；进入游戏、退出框 B 取消、
A Quit 均正常，exit=0。证据目录包含 `before/`、`fixed5/`、`fixed10/`、`normal/`、
存档测试样本及反汇编；测试后已恢复原容器存档。测试仅修改容器存档，
未修改掌机存档。修复版已于 2026-10-15 部署到 172.16.7.34；远端
`SHA256SUMS` 的 145 项全部校验通过，`brickgamepro.so` 哈希为
`a9f3e2b87ef297b10de05cdc1cd47acae5fd1ebb3bef262a001b0b0026197ca8`。
远端旧插件与清单保存在 `backup-startup-input-20261015/`，本次未远程启动游戏，
仍需从掌机菜单进行实际按键验证。

2026-10-12。源码位于 `projects/unityloader/plugins/brickgamepro/brickgamepro.cpp`。
本轮同时修正 [Down 长按只作用于当前方块](BRICKGAMEPRO_DOWN_ANALYSIS.md)，
保留 100 ms / 30 ms 参数及已部署的字体栅格密度修复。

## 原版证据

分析同一 APK 的 ARM64 `libil2cpp.so`、Il2CppDumper 元数据和 Unity `level0`。
方法地址是 RVA，插件按方法/字段名解析，不硬编码地址。

| 方法 | RVA | 用途 |
| --- | --- | --- |
| GUIManager.Update | `0x247E7C` | Unity 主线程处理结束界面关闭请求 |
| GUIManager.BackSelectGameMenu | `0x247F30` | 原 Menu 按钮关闭当前菜单并恢复选游戏状态 |
| CanvasRenderer.SetColor_Injected | `0x6E729C` | 最后一级颜色写入，阻止触摸按钮恢复可见 |
| CanvasRenderer.GetColor | `0x6E7318` | 读取实际 renderer 颜色以验证 alpha |

`level0` 的 `FinishMenu/BackToMenu` Button 序列化数据含
`BackSelectGameMenu` onClick 方法名。反汇编确认该方法处理 `currentMenu`
（offset `0x408`）、`blankBtn`（`0x28`）并在 `0x248140` 把 `menuStatus`
（`0xC0`）写为 SelectingGame（2）。直接关闭 FinishMenu GameObject 会遗漏这些状态。

FinishMenu 直接子节点除标题、比分外还有 `ShareBtn`、`LeaderBoardBtn`、
`BackToMenu`、`RePlayBtn`、`GiftBtn` 和 `LeaveFeedBackBtn`。只匹配这个父节点
下的六个动作，避免误关其他界面同名节点。

## 实现

GUIManager.Update 发布 FinishMenu 的实际可见状态。结束界面下 A 的按下只
排关闭请求，下一次 Unity Update 调用原 BackSelectGameMenu；对应 A 松开
由这个操作消费，避免漏入随后游戏。调用 managed 函数不在 SDL 回调线程执行。

六个结束界面动作设为 inactive。GameObject.SetActive hook 仅对它们禁止
重新激活；Game Over 标题与比分继续显示。

以前依靠触摸后/每帧再设透明，Unity Button 颜色过渡仍可在两次刷新间恢复
非零 alpha，出现闪现。现在发现控制节点及后代的 CanvasRenderer，并在
SetColor_Injected 写入时把这些 renderer 的 alpha 固定为零。拷贝 ref Color，
不修改 tween 自己持有的颜色数据。控制布局与 raycast 保留，实体键仍可触发
原按钮；隐藏列表不包括 HUD、棋盘及背景。

`BRICKGAME_UI_TRACE=1` 输出前 24 次被阻止的非零颜色写入，每 120 帧
读取真实 renderer 颜色及结束动作 activeInHierarchy。部署默认不开该诊断。

## 容器验证

GlES_Dev、AArch64/QEMU、Xvfb、640×480、software GL。原 GUI 入口启动，
`viaGUI=1`；预设 timeout 导致 exit=124，另行校验崩溃标记。

| 案例 | 结果 |
| --- | --- |
| finish | 自然 Game Over；A 仅关闭一次并恢复 menu=2；随后 Start 重新开始、Down 正常；A 无触摸事件泄漏 |
| buttons | Start 暂停/恢复、X 音效、Select 换皮肤、Guide 菜单、A 和 Right 正常；24 次非零 alpha 写入被阻止 |

两组共 16 次可见性采样，每次 19 个 renderer 的 alpha 全零，六个结束动作
均 inactive。已查看 Game Over、A 后选游戏、暂停和换皮肤后的截图：结束界面
底部按钮不显示，控制图形没有闪现，比分文字清晰。

另外八组 Down 回放通过普通/Five 换块长按停止、松开重按、短按、Game Over
后松键、暂停恢复、横移旋转组合及 Tank 原触摸路由。真实掌机效果仍需用户测试。

证据保存在本地 ignored staging：
`deploy/BrickGamePro/ui-validation/gameover-and-hidden/{finish,buttons}`，
含截图、timeline、日志和 `validation-summary.json`；原版关闭函数反汇编在
`ui-validation/il2cpp-evidence/BackSelectGameMenu.txt`。

重现（先构建并更新容器 game 中的插件）：

```powershell
docker cp scripts/container-brickgamepro-ui-matrix.sh GlES_Dev:/tmp/brickgame-ui-matrix.sh
docker exec GlES_Dev bash /tmp/brickgame-ui-matrix.sh
docker cp scripts/verify-brickgamepro-ui.py GlES_Dev:/tmp/verify-brickgamepro-ui.py
docker exec GlES_Dev python3 /tmp/verify-brickgamepro-ui.py /game/BrickGamePro
```

UI 和 Down 回放共享 Xvfb 与 log-seq.txt，必须顺序运行。
UI 脚本可传 `finish`、`buttons` 或 `exit` 单独重跑对应案例；省略时运行全部三组。

## 2026-10-13：退出确认框与 Select

`level0` 的原始 UnityEvent 绑定已离线解析确认：`ExitMenu/YesBtn` 调用
`GUIManager.ExitGame`，`ExitMenu/NoBtn` 调用 `BackSelectGameMenu`。
反馈框也有同名 YesBtn/NoBtn，因此按父节点 ExitMenu 绑定确认框对象。
IL2CPP `ExitGame` RVA `0x24BB6C` 尾跳 `0x395FDC`，后者是
`UnityEngine.Application.Quit()`；退出使用原版流程并保存 PlayerPrefs。

插件在 GUIManager.Update 发布 ExitMenu 的 activeInHierarchy。SDL 中 A/B
只排队，Unity 线程执行原方法；B 关闭后仍吞掉对应松键，避免事件泄漏。
该路由优先于 Game Over A 路由。B 在确认框外保持未映射。
Select 换皮肤路由及 UnitySendMessage 调用已在代码整理时删除；Start+Select 退出组合保留。
用户随后取消图片修改，本次没有替换或部署任何皮肤资源。

三组容器回放验证通过：

| 案例 | 本次结果 |
| --- | --- |
| exit | 主菜单 Guide 打开；B 关闭且 menu=2；再次 Guide 打开；A 原版 Quit，exit=0，保存设置；A/B 不泄漏触摸 |
| buttons | Select 回放不再产生 skin message；Start/X/Guide/A/Right 路由正常；24 次非零 alpha 写入被阻止 |
| finish | Game Over 按 A 返回 menu=2，随后 Start/Down 正常，无 A 触摸泄漏 |

22 次 UI 采样中，19 个 renderer 均 alpha=0，6 个 Game Over 动作均 inactive。
finish/buttons 的 exit=124 是测试时限，exit 案例实际由 A 正常退出，exit=0。
证据：`deploy/BrickGamePro/ui-validation/exit-select/{exit,buttons,finish}` 及
`validation-summary.json`。复现脚本和校验器现包含 exit 案例。

本次部署插件 SHA-256：
`740CB9A5DEE502E86C2C79EAFDC36C9A921A655B2F39E28E450A213C7F1ED881`，
Build ID `f9efa6473e2e8b82abace2857df31604638d5855`。
Release、LOG=ON，其余诊断关闭，strip；RETURN_NON_ZERO=OFF 已核对。
本地包仍为 57,139,200 字节、144 个运行文件，SHA-256：
`7D4E50BDF06E5B158D9ADA29D8B08F53877CC635C2A01E69F9EFC2404FCF0A41`。

已推送 `172.16.5.40:/mnt/mmc/Roms/PORTS/BrickGamePro`，先确认无游戏进程，
旧 F5815C86… 插件备份至根目录 `brickgamepro-ui-held-piece-backup.so`。
新插件及清单先上传 `.new` 并验证哈希后替换，144 个运行文件全部通过校验。
掌机 `deploy-verify.txt` 保存验证结果；没有远程启动游戏，待用户菜单测试。

## 前一版本产物与部署状态（Game Over 与换块 Down）

Release、LOG=ON、TRACE/VERBOSE/IL2CPP_TRACE=OFF、strip，显式
`JNIVM_ENABLE_RETURN_NON_ZERO=OFF`；DEBUG=ON、JNIVM TRACE=ON 已核对缓存。
构建目录、容器 game 和本地 staging 的插件 SHA-256 一致：
`F5815C8610E2D00124C03510F3FA84D88F8C863B997A04889DE9E8DCF58EA171`。
Build ID：`227e774e012ca00db5a7f8cb3762f34cd4bdb4a1`。

本地 `deploy/BrickGamePro-handheld.tar` 已重打包，57,139,200 字节、144 个运行文件，
SHA-256：`F92C968DD695F927C6781570B437A46A004B9DDD44B267374EF71D16505ABB9E`。
只需更新插件及 SHA256SUMS，loader、配置和游戏资源均无新增变化。

2026-10-12 掌机连接超时，未部署；2026-10-13 用户确认上线后完成更新。
推送前确认无 unityloader 进程，旧插件为 `E6D10F2A…` 的 100/30 + 字体版。
旧插件备份到游戏根目录 `brickgamepro-100-30-font-backup.so`，不进入插件扫描目录。
插件和 SHA256SUMS 均先上传 `.new`，插件 SHA-256 验证后替换；掌机全部
144 个运行文件通过清单校验，新插件哈希与上述容器验证产物一致。
没有远程启动游戏，由用户从 PORTS 菜单验证实际效果。

## 2026-10-13：代码归属整理与废弃逻辑删除

通用 IL2CPP 导出绑定、元数据查询、方法 detour 与类诊断移至
`projects/unityloader/plugin_il2cpp.h`，仍使用现有 v8 C ABI。
BrickGame 的字段布局校验、类名、坐标、Down 节奏、字体及 UI 策略留在插件。
删除 Select 换肤/UnitySendMessage、整套 direct-input/pulse 队列、世界对象缩放、
Grid extra-scale、无效开关/状态字段和未安装的玩法探针。保留实际回放使用的诊断入口。

Release loader 与插件构建通过，额外 `-Wall -Wextra -Wunused` 语法检查无警告。
LOG=ON，TRACE/VERBOSE/IL2CPP_TRACE=OFF，RETURN_NON_ZERO=OFF，strip。
完整八组 Down 与三组 UI 校验通过；最后删除恒真开关和只写状态字段后，
重跑 fixed 与全部三组 UI，通过同一校验器。最终 UI 共 22 次采样，19 个
renderer 均 alpha=0、6 个结束动作均 inactive；exit 案例 A 正常退出 exit=0。

最后补跑中有一次 exit 停在 SDL 创建窗口前，未输出 OpenGL Renderer、
GUI Start 或 UI 采样，校验器按缺少采样拒绝。相同二进制单独重跑 exit 通过，
未修改游戏代码或放宽断言。失败证据保存在 `cleanup-validation/startup-stall/exit/`；
该容器偶发启动停滞的原因尚未进一步定位。

最终证据：本地 ignored `deploy/BrickGamePro/cleanup-validation/final/{down,ui}/`，
含日志、截图、timeline、runner 与 validation-summary.json。
最终插件 SHA-256：
`FDC9C8F614399DDC2F8BFD69C1385CC6BF783FF714480AD3E175E51BB7FE6FD4`，
Build ID `fe3fea0c96181e60283f16be3f16818e61999529`。
本地安装包 57,159,680 字节、144 个运行文件，SHA-256：
`B76251A961FD8A90E90FBF45770D0411C348656021D00A5745AE53FE5DDDE4FF`。
本轮只更新本地 staging 和安装包，掌机仍使用上文已部署的 `740CB9A5…` 版本。
