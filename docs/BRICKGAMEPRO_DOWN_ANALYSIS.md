# BrickGamePro Down：IL2CPP 分析与容器修复验证

2026-10-12。修复位于 `projects/unityloader/plugins/brickgamepro/brickgamepro.cpp`，
已在 `GlES_Dev` ARM64/QEMU 容器构建并验证，早期各版已部署到 `172.16.5.40`。
最新单块长按/UI 版已于 2026-10-13 掌机恢复联网后部署，详见文末。
掌机运行及真实手感仍待从菜单启动验证。
当前节奏采用用户最新指定值：下一次 Update 立即执行一格，首次重复等待 100 ms，
随后每 30 ms 一格；松开停止重复，极短按保留一次动作。
长按只作用于当前方块：换块时取消重复，下一块自然下降，必须松开再按才能加速。
初版 200 ms / 90 ms 经用户真机测试确认手感提升，本次继续提高灵敏性。

## 原 APK 与分析方法

APK：`C:\Users\Administrator\Desktop\BrickGame\BrickGamePro\brickgamepro_421839.apk`。
分析的是 `lib/arm64-v8a/libil2cpp.so` 和
`assets/bin/Data/Managed/Metadata/global-metadata.dat`，不是仅根据输入桥代码推测。

| 原始文件 | SHA-256 |
| --- | --- |
| libil2cpp.so | `EF28DC6D9AF55B12B82FA5CF941EFE434CF47D5FB3443CC92974FFAB587BB143` |
| global-metadata.dat | `E874F7EEB0CBC29D79910C34C92BF4CE949C4E5DE02034B4827139BDB2636B09` |

Il2CppDumper 输出提供类、字段、方法与 RVA；再用 NDK `llvm-objdump` 对照 ARM64
指令恢复实际控制流。`dump.cs` 本身不包含方法体，不能作为计时行为的证据。
工具位置：

- Il2CppDumper：`E:\BaiduNetdiskDownload\Games\FiveHeartsProject\Docs\tools\il2cppdumper\bin\Il2CppDumper.exe`。
- objdump：`C:\dev\android-ndk-r27d\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-objdump.exe`。
- 原始反汇编副本：`deploy/BrickGamePro/down-fix-validation/il2cpp-evidence/`。

以下地址都是本 APK 的 RVA，运行地址需加模块基址。插件按方法和字段名解析，
不直接硬编码这些函数地址。

| 方法 | RVA | 作用 |
| --- | --- | --- |
| GameManager.StartMoveDown / EndMoveDown | `0x6B4408` / `0x6B44BC` | 仅写共享输入状态 3 / 6 |
| TetrisMino.Start / Update | `0x6B68E8` / `0x6B6994` | 初始化；Update 尾跳到 CheckUserInput |
| TetrisMino.CheckUserInput | `0x6B6998` | 横移、旋转、Down 和自然重力入口 |
| TetrisMino.MoveDown | `0x6B795C` | 一格位移及原碰撞、回退、落地、换块处理 |
| TetrisMino.MoveDownSingleBrick | `0x6B72E4` | 单格特殊碰撞路径 |
| TetrisFiveMino.Start / CheckUserInput | `0x6AC6C4` / `0x6AC8A8` | Five 的独立类与输入入口 |
| TetrisFiveMino.MoveDown | `0x6AD5C0` | Five 的独立下降实现 |
| GUIManager.PressStartPauseGame | `0x248DC4` | 菜单、选游戏、暂停状态转换 |
| GameManager.FinishGame | `0x23AD58` | 清理游戏结束状态 |

## 手感问题的实际原因

`StartMoveDown` 只把 mino 的 `tetrisMinoStatus` 写为 3，不会立即调用下降；
`EndMoveDown` 写为 6。该状态还被横移和旋转共用，不能独立表示所有同时按住的按钮。

`CheckUserInput` 在自然重力到期、键盘 Down 按住或状态为 3 时进入 MoveDown。
其末尾 `0x6B6C04` 根据 `isSingle` 选择普通或单格下降。
MoveDown 内部的关键顺序是：

1. `moveImmediateVertical` 为真时清标志并直接下降。
2. 否则先累计共享的 `verticalButtonDownTimer`，达到约 0.2 秒才进入后续分支。
3. **状态为 3 或键盘 Down 按住时，直接跳过 `verticalTimer` 的约 0.03 秒门限。**
   `0x6B7EE8` 读取状态；`0x6B7EF0` 的 `b.eq 0x6B79B8` 跳回实际下降路径。
   键盘分支 `0x6B7F28` 也跳到同一位置，计时检查在它们之后。
4. 下降路径自己更新 `fall` 并处理碰撞；失败时回退位置，然后走原落地流程。

因此连续 Down 在初始门限后可每次 Update 都下降一格，速度随帧率变化；
短按又可能仅推进共享门限，松开后没有留下一次独立动作。
延长触摸存活时间和持续发送 ACTION_MOVE 没有改变这个 managed 控制流，
不足以稳定解决手感问题。

TetrisFiveMino 也有同类旁路：`0x6ADA40 -> 0x6AD610`。
MoveDownSingleBrick 保留类似计时门限，但碰撞行为不同，不能用直接修改坐标替代。

| 字段 | TetrisMino offset | TetrisFiveMino offset |
| --- | --- | --- |
| tetrisMinoStatus | `0x18` | `0x18` |
| fall | `0x1C` | `0x2C` |
| isSingle | `0x23` | 不使用 |
| moveImmediateVertical | `0x39` | `0x31` |
| verticalButtonDownTimer | `0x40` | `0x38` |
| verticalTimer | `0x48` | `0x40` |

## 修复边界与实现

插件解析字段并核对上述输入门限布局和 `GameManager.playGameStatus == offset 0x90`。
依赖不可用或布局不同则不启用接管。普通类覆盖游戏枚举 1、10、11、12、14、19，
Five 为 13；其他游戏和菜单使用现有触摸映射。旧的整套 direct managed input
实验已在代码整理时删除，A/Y、横移、Start 等继续经过触摸。

- SDL 回调只记录 Down 按下/松开；managed 字段读写与 MoveDown 调用在原
  `CheckUserInput` 所在游戏线程执行。
- 短按用队列保存，即使 Down/Up 都发生在一次 Update 前也保留动作。松开清重复，
  不删除尚未执行的合法短按。
- 采用 Unity `Time.time` 调度：首动作后等待 100 ms，后续等待 30 ms。
  每 Update 至多一次，慢帧不会补发一串动作，所以实测间隔可能大于目标。
- 每次只临时设置 Down 状态、immediate 标志和两个门限计时器，再调用原 MoveDown；
  `isSingle` 分派到原 MoveDownSingleBrick。之后恢复共享输入状态，保留横移/旋转，
  重置 vertical gate，让下一次原自然重力可以正常执行。
- 原下降函数处理坐标、合法性、落地、消行和换块。已禁用的落地对象不再执行
  本次原 CheckUserInput；Start hook 记录最新生成对象，避免已落地但仍 Update 的
  特殊对象消费新输入。
- 原 `TetrisMino.Start` 在 `0x6B6928` 将 status（offset `0x18`）写为 0。
  新对象不继承旧块的 Down 状态。插件在普通/Five 的 Start 中同时取消重复和旧队列，
  保留真实 Down held/owned，重复按下事件不会重新启用加速，必须先 UP 再 DOWN。
- 锁覆盖一次 managed 动作，UP 返回后不会再启动旧重复。
  锁为 recursive mutex，因为原 MoveDown 可以同步重入 FinishGame。
- 暂停时 Down 被接管但不排动作；暂停期间按住的键不会在恢复后漏入触摸。
  暂停中止重复，恢复后需要一次新的 Down 按下。FinishGame 清队列、重复与活动对象；
  按键路由所有权保持到真实 UP，避免同一按键两条路径重复释放。
- 接管还要求最近 250 ms 有对应 mino Update，防止使用过期游戏状态。

诊断开关：`BRICKGAME_DOWN_FIX=0` 用原触摸 Down 做 A/B；
`BRICKGAME_DOWN_TRACE=1` 输出 edge/step/original/spawn/finish。
FIX=0 且 TRACE=1 仅监测原逻辑，不改变下降字段。

## 初版 200 ms / 90 ms 容器实测（历史记录）

ARM64/QEMU、Xvfb、software GL，640×480。所有案例经原 GUI 入口启动，
日志确认 `viaGUI=1`。直接调用 GameManager.Play* 会留下 GUI 选游戏状态，
导致后续 Start 重新开局，因此没有用早期该入口的日志证明暂停行为。
各运行的 exit=124 是预设 timeout 结束；校验另外检查了 SEGV、terminate、
Invalid Reference 等崩溃标记。

| 案例 | 输入 | 实际结果 |
| --- | --- | --- |
| baseline | 原逻辑 Down 1 秒 | 连续 11 次位移，间隔 24–59 ms，中位 38 ms |
| fixed | 修复 Down 1 秒 | 9 次动作；首次重复 201 ms，随后 96–124 ms；UP 后无重复 |
| lifecycle | Down 30 秒 | 89 次动作，8 个 mino；进入 Game Over，之后无 step，UP 正常处理，无死锁 |
| five-final | Five Down 6 秒 | 54 次动作，跨 4 个 mino；UP 后无重复 |
| taps-final | 12 次、每次请求 hold 1 ms，间隔 150 ms | 12 次 tap 全处理；11 次一格位移，第 12 次碰撞落地并生成下一块 |
| gui-pause | Down + Right + A；Start 暂停/恢复 | 原横移从 x=5 到 6，原旋转入口有效；暂停 Down queued=0，无 step；恢复后新 Down 正常 |
| tank-final | Tank Down 1 秒 | 保持原 down/up touch 路径，无 Tetris Down step |

日志时间取 Unity `Time.time`，保留三位小数；hold=1 ms 是输入回放请求值，
实际 SDL 边沿受调度影响（本次约 4–6 ms）。每步位置变化仅为一格或原碰撞回退。
长按跨块时包含生成等待及慢帧：lifecycle 间隔 90–205 ms，Five 为 90–220 ms。
这些结果证明重复不会快于约 90 ms，不等于容器能精确提供每 90 ms 的墙钟动作。
松开后的原自然重力仍可见（fixed 日志 y=4→3→2）。

校验器：`scripts/verify-brickgamepro-down.py`；完整回放：
`scripts/container-brickgamepro-down-matrix.sh`。结果、截图、反汇编和
`validation-summary.json` 保存在本地 ignored staging：
`deploy/BrickGamePro/down-fix-validation/`。
已检查 fixed、Five、暂停和 Game Over 截图，棋盘正常显示。

## 构建与重现

容器源码不是宿主 bind mount，需先复制修改。每次 configure 显式关闭
`JNIVM_ENABLE_RETURN_NON_ZERO`，避免复用缓存造成假崩溃。

```powershell
docker cp projects/unityloader/plugins/brickgamepro/brickgamepro.cpp GlES_Dev:/workspace/Bogodroid/projects/unityloader/plugins/brickgamepro/brickgamepro.cpp
docker cp projects/unityloader/plugin_il2cpp.h GlES_Dev:/workspace/Bogodroid/projects/unityloader/plugin_il2cpp.h
docker exec GlES_Dev bash -lc 'cd /workspace/Bogodroid && cmake -S . -B build-device-release -DCMAKE_BUILD_TYPE=Release -DBD_ENABLE_LOG=ON -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF -DIL2CPP_TRACE=OFF -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF && cmake --build build-device-release --target brickgamepro.so -j2 && aarch64-linux-gnu-strip --strip-unneeded build-device-release/unityloader.d/brickgamepro.so && cp build-device-release/unityloader.d/brickgamepro.so /game/BrickGamePro/unityloader.d/brickgamepro.so'
docker cp GlES_Dev:/workspace/Bogodroid/build-device-release/unityloader.d/brickgamepro.so deploy/BrickGamePro/unityloader.d/brickgamepro.so
docker cp scripts/container-brickgamepro-down-matrix.sh GlES_Dev:/tmp/brickgame-down-matrix.sh
docker exec GlES_Dev bash /tmp/brickgame-down-matrix.sh
docker cp scripts/verify-brickgamepro-down.py GlES_Dev:/tmp/verify-brickgamepro-down.py
docker exec GlES_Dev python3 /tmp/verify-brickgamepro-down.py /game/BrickGamePro
```

从 `/game/BrickGamePro/seq-down-<case>` 分别复制 8 个案例到
`deploy/BrickGamePro/down-fix-validation/held-piece-100-30/<case>`，然后：

```powershell
& 'C:\dev\Python310\python.exe' scripts/verify-brickgamepro-down.py
```

校验器默认校验当前 100 ms / 30 ms 及单块长按规则。
检查旧七组跨块连续加速的历史日志时，必须显式传旧结果目录和
`--legacy-continuous-hold`，初版还需 `--initial-ms 200 --repeat-ms 90`，
100 ms / 60 ms 历史版还需 `--repeat-ms 60`。

校验器可另传结果根目录路径；必须不用 Python `-O`。回放脚本每次覆盖该案例目录，
并立即归档共享 log-seq.txt。预计约 4 分钟，测试中不会启动掌机。

初版产物 `deploy/BrickGamePro/unityloader.d/brickgamepro.so` 为 stripped AArch64 ELF。
当时构建、容器 game 和宿主 staging 三份 SHA-256 一致：
`32849F070CAE07314AF8815BC3534409476336F550960CC868451A7E6C1B6C71`。
Build ID：`e223a986dfb375de1f307e91c8ebee08945dff70`。
Release、LOG=ON、TRACE/VERBOSE/IL2CPP_TRACE=OFF、RETURN_NON_ZERO=OFF 已核对缓存；
JNIVM DEBUG=ON、TRACE 随 LOG=ON，遵循项目规则。

## 尚未验证的部分

初版 200 ms / 90 ms 已获用户真机反馈：Down 手感有提升。随后先调整为
100 ms / 60 ms，本轮按用户要求改为 100 ms / 30 ms；当前参数的物理按键
延迟和手感仍待真机反馈。
独立单格、炸弹、反向玩法以及消行场景未专门回放；其原函数与碰撞调用链保留，
但不能把普通 Tetris/Five 的通过结果当成这些场景的实测证据。
首轮容器矩阵出现 PlayerPrefs `.tmp` 保存失败警告；部署准备中确认是
`init_config` 已切换到 `game/`，原 `./conf/` 却指向 `game/conf/`。
配置改为 `../conf/`，并创建 `conf/shared_prefs/` 后的补充容器回放成功保存
PlayerPrefs（1 record），未再出现该失败。随后掌机退出日志也确认保存成功。

## 掌机部署记录（2026-10-12）

目标 `172.16.5.40`，AArch64、Mali-G31；参照该机 PORTS 下现有的
Maximus2/Samurai2 脚本，而不是套用 TrimUI 的系统路径。

- 游戏目录：`/mnt/mmc/Roms/PORTS/BrickGamePro`。
- 菜单入口：`/mnt/mmc/Roms/PORTS/BrickGamePro.sh`。
- 日志：`/mnt/mmc/Roms/PORTS/BrickGamePro/log.txt`，启动脚本每次重新写入。
- 脚本源：`scripts/brickgamepro_launch.sh`，bash 语法检查通过，loader 和入口
  已赋可执行权限。默认启用 Down 修复，不启用自动进入游戏或步骤 trace。
- 完整包 57,139,200 字节，通过 Dropbeak `--force --chunk --chunk-size 16m --verify`
  上传；解包后所有 144 个运行文件通过 `sha256sum -c SHA256SUMS`。
- loader 和插件的 `ldd` 均正常解析，未发现缺失依赖。
- 插件哈希与上述容器验证版本一致；loader SHA-256 为
  `BB4F21CF894AD4CCB477DC81B51B42DA056E74E29AE1546F8415E5A4EC224FB6`。
- 入口 SHA-256：`26B3A83862310A8E0DAF49E9C862C6ECD1CF5050943D1E0970163925F28B5B69`。
- 配置 SHA-256：`DA9EB88AFF6B6C17BCC3F2A97851F9E6B1704B5FE9FB9796E71FD9B4F3B86CA3`。
- 上传临时 tar 已清理；本地 staging 保留安装包、SHA256SUMS 和容器回放证据。

本次没有远程执行游戏入口。上机从 PORTS 菜单选择 BrickGamePro，进入 Tetris，
重点检查短按一格、长按首次延迟、松手停止，以及 Down 同时横移/旋转。

## 100 ms / 60 ms 与字体修复组合版（2026-10-12）

代码保持下一次 Update 立即处理首步，首次重复阈值改为 100 ms，后续阈值
改为 60 ms。构建同时包含 [字体栅格密度修复](BRICKGAMEPRO_FONT_ANALYSIS.md)。
首次新节奏矩阵中 Five 从直接 Play 入口启动，未满足 `viaGUI=1`，因此未作为
通过结果。测试入口改为等待 Logo 到 SelectingGame 的异步转换，再经原 GUI
启动；最终七组全部经 GUI 启动，校验器在容器及宿主均通过。

| 案例 | 组合版实测结果 |
| --- | --- |
| baseline | 13 次原位移，间隔 28–87 ms，中位 44.5 ms |
| fixed | 12 次动作；首重复 128 ms，随后 81–88 ms；UP 后无重复 |
| lifecycle | 112 次动作，10 个 mino；Game Over 后无 step，UP 正常 |
| five-final | 69 次动作，5 个 mino；重复间隔 61–123 ms |
| taps-final | 12 次请求 hold=1 ms 短按全部处理，本轮均为一格位移 |
| gui-pause | 横移旋转组合及暂停恢复通过；重复间隔 60–120 ms |
| tank-final | 原 Down/Up touch 路径保留 |

Unity 动作依赖帧调度，容器软件渲染下实测间隔会大于配置阈值；这些结果
不代表真机按键到画面的墙钟延迟。最终日志、截图和 JSON 在
`deploy/BrickGamePro/down-fix-validation/sensitivity-100-60-font/`；更早
未通过的尝试单独保存在 `sensitivity-100-60/`，不混入最终证据。

组合版插件 SHA-256（构建、容器 game、宿主 staging、掌机一致）：
`EEEC0533F08F4C813FEABC09E6509244BABAAD721F3597139724A1C91F266C75`。
本地安装包重新生成，含 144 个运行文件；包 SHA-256：
`A997452B3BE8D45B9B9BA34357A783F2315DD126E5B4B8850E1DA8C5440AB6AD`。
旧 200/90 插件在掌机游戏目录下保留为 `brickgamepro-200-90-backup.so`，
位于插件扫描目录外。仅更新插件及 SHA256SUMS；没有远程启动游戏。

## 首次 100 ms / 30 ms 版（2026-10-12，历史记录）

按用户要求保留首步在下一次 Update 处理、首次重复阈值 100 ms，将后续
重复阈值由 60 ms 降为 30 ms。只影响 TetrisMino 玩法（游戏 ID
1、10、11、12、14、19）和 TetrisFiveMino（13）；菜单与其他游戏维持原 Down
touch 路径。保留字体修复。Release / LOG=ON / strip，RETURN_NON_ZERO=OFF。

七组容器回放全部通过，容器与宿主校验器结果一致：

| 案例 | 本轮实测结果 |
| --- | --- |
| baseline | 13 次原动作，间隔 27–95 ms，中位 44.5 ms |
| fixed | 16 次动作，2 个 mino；首次重复 133 ms，随后 41–73 ms；UP 后无重复 |
| lifecycle | 104 次动作，10 个 mino；Game Over 后无 step，UP 正常 |
| five-final | 110 次动作，10 个 mino；重复间隔 39–152 ms |
| taps-final | 12 次 hold=1 ms 短按全部处理，12 次一格位移 |
| gui-pause | 横移旋转组合及暂停恢复通过；重复间隔 35–124 ms |
| tank-final | 原 Down/Up touch 路径保留，无插件 Down step |

每次 Update 最多一步，实测间隔受容器帧调度限制，不代表真机延迟。
日志、截图和 JSON 归档到
`deploy/BrickGamePro/down-fix-validation/sensitivity-100-30-font/`。

插件 SHA-256（构建、容器 game、宿主 staging、掌机一致）：
`E6D10F2AA854BA8DC8E1856EDDAF1CB7F52BB2F57E27DE7596827E8DF335285F`。
Build ID：`53468d2ab3ecb7c1dc96cd04e24ce6ddc06e566c`。
安装包重新生成，57,139,200 字节、144 个运行文件；包 SHA-256：
`79506800F36587D5C5719E95B6F10A16E665322732C8DA2EC751146CF4A566FC`。

掌机旧组合版备份为游戏目录根下的 `brickgamepro-100-60-font-backup.so`，
位于插件扫描目录外。推送 `.new` 文件并核对 SHA-256 后替换，随后更新
SHA256SUMS，掌机 144 个运行文件全部校验通过。未远程启动游戏，当前
100/30 手感及字体效果待用户从 PORTS 菜单验证。

## 单块长按规则 + Game Over/UI 版（2026-10-12）

用户补充原版规则：按住 Down 只加速当前块，落地后下一块慢速下降，必须
松开重按才能加速。插件因此在普通/Five 新块的 Start 中清旧 tap 队列并取消
repeat armed，保留 SDL held 和路由所有权直到松键。参数继续为 100 ms / 30 ms。
同时包含 [Game Over 按 A 关闭与持续隐藏触摸按钮](BRICKGAMEPRO_UI_ANALYSIS.md)。

八组矩阵通过，宿主/容器校验一致；每次 Down 按住期间只出现一个对象的手动 step：

| 案例 | 结果 |
| --- | --- |
| baseline | 原 Down 12 次位移，间隔 14–58 ms，中位 39 ms |
| fixed | 13 次动作，仅一个 mino；首重复 105 ms，后续 35–58 ms；松开无 repeat |
| spawn-gate | 两次 10 秒长按共 18 次动作、两个加速对象；每次下一块自然下降，松开重按后才重新加速 |
| five-final | 两次 6 秒长按共 22 次动作、两个加速对象；同样不跨块连续加速 |
| lifecycle | 63 次动作、8 个加速对象；自然 Game Over 后无 step，真实 UP 仍正确处理 |
| taps-final | 12 次极短按全部处理；11 次一格位移，1 次原碰撞落地 |
| gui-pause | 横移/旋转及暂停恢复通过；暂停 Down queued=0，无 step |
| tank-final | 保留原 down/up touch，未出现插件手动 step |

第一次 lifecycle 使用每次 hold=2 秒，UP 发生在 Game Over 动画完成前，
没有覆盖 FinishGame 后松键这一断言。仅该案例延长为 hold=4 秒并重跑，
上述断言通过；未放宽校验条件。原未覆盖版本单独保存在
`down-fix-validation/lifecycle-hold-too-short/`。

最终日志、截图与 JSON：`deploy/BrickGamePro/down-fix-validation/held-piece-100-30/`。
完整矩阵现在八组；脚本允许附加案例名（如 `lifecycle`）只重跑指定案例。
容器每 Update 至多一步，实测间隔不等于真机输入延迟。

插件 SHA-256：`F5815C8610E2D00124C03510F3FA84D88F8C863B997A04889DE9E8DCF58EA171`，
Build ID：`227e774e012ca00db5a7f8cb3762f34cd4bdb4a1`。
本地安装包和清单已重生成。2026-10-12 掌机连接超时，2026-10-13 恢复联网后
完成部署，新插件哈希一致，全部 144 个运行文件通过清单校验。
旧 `E6D10F2A…` 插件备份在游戏根目录 `brickgamepro-100-30-font-backup.so`。
未远程启动游戏；更新步骤与安装包哈希见 [UI 记录](BRICKGAMEPRO_UI_ANALYSIS.md)。
