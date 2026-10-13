# BrickGamePro：Game Over 与触摸按钮隐藏修复

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
