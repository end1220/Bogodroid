# Space Squids 移植与手柄适配

## 当前状态

支持 `com.bolopix.calamarispaziali` 0.1，Unity 2022.2.14f1 / ARM64 IL2CPP。
2026-10-22 已完成容器验证及掌机测试，用户确认当前版本通过。
游戏插件位于 [`projects/unityloader/plugins/spacesquids/`](../projects/unityloader/plugins/spacesquids/)。

2026-10-22 后续加入通用 [Android 震动桥接](HAPTICS.md)，承接游戏原生的
`Vibrator` 请求与震动开关。2026-10-23 用户确认掌机震动有效，随后按要求
构建关闭日志的发行版本；验证、部署和回退信息见震动文档。

战斗使用原生摇杆移动和原游戏射击函数；菜单通过光标注入触摸。
设置、飞船列表、暂停、死亡弹窗均已覆盖。光标是白色短十字：内层 15×15、
臂宽 5 像素，深色描边 19×19、臂宽 9 像素，尺寸按最终 drawable 像素绘制。
死亡后光标停在**屏幕内顶部空白处**，不是屏幕外。

## 操作

按键名称指 SDL 逻辑按钮，物理位置以随包的 `gamecontrollerdb.txt` 为准。

| 场景 | 按键 | 行为 |
|---|---|---|
| 菜单 / 设置 / 暂停 | 方向键、左摇杆 | 向指定方向吸附到可见控件，长按重复 |
| 菜单 | 右摇杆 | 自由移动光标 |
| 菜单 | A | 在光标位置触摸；按住 A 再移动可拖动 |
| 菜单 | B | 点击当前页 Back / Resume；没有对应按钮时无操作 |
| 主菜单 | Guide | 点击 OptionsButton 打开设置 |
| 飞船列表 | 左右方向键、左摇杆 | 逐艘选择并滚动居中，支持全部 11 艘；到首尾不循环 |
| 飞船列表 | 上、B | 上可导航到返回按钮；B 直接返回 |
| 飞船列表 | A / 按住 A 移动 | 点击卡片，或使用原版触摸拖动 |
| 设置滑块 | A | 光标落在当前滑块手柄，点击不跳到中点 |
| 设置滑块 | 按住 A + 左右 | 拖动滑块，可到达 0% 和 100% |
| 死亡弹窗 | 方向键选择，再按 A | 初始空白位置不响应 A，B 无操作；支持金币复活、Restart、Home |
| 战斗 | 方向键、左摇杆 | 原生 Unity Horizontal / Vertical 飞行 |
| 战斗 | A / RB / RT | 按住普通开火，共用原游戏冷却，不叠加射速 |
| 战斗 | X | 每次按下发射一轮导弹，长按不连发 |
| 战斗 | Start | 点击 PauseButton 暂停；暂停后 B 恢复 |
| 全程 | Start + Select | loader 退出组合键，UI 捕获期间仍可用 |

死亡弹窗出现时，之前按住的键必须松开。用方向键选中按钮或右摇杆移到目标后，
再按 A 才会触发。金币不足时是否可复活，由原游戏判断。

## 安装与配置

loader 和插件必须配套。插件使用 ABI v8 新增的可选尾部
`set_controller_capture` / `set_present_cursor`，初始化会检查 `struct_size`。
旧 v8 字段偏移没有改变，但旧 loader 不具备新尾部，不能搭配此插件。

```text
SpaceSquids/
  unityloader
  unity.toml
  gamecontrollerdb.txt
  unityloader.d/spacesquids.so
  game/                         # 自备原版资源和 ARM64 库
    lib/arm64-v8a/{libunity,libil2cpp,libmain}.so
    assets/bin/Data/...
    UnityDataAssetPack.apk
  conf/                         # IL2CPP 元数据及运行时存档
  cache/
```

参考 [`unity.toml.example`](../projects/unityloader/plugins/spacesquids/unity.toml.example)。
资源需从合法取得的同版本 APK / 资源包准备；仓库不包含游戏资源或用户存档。
不要覆盖已有 `conf/`。启动时以游戏目录为工作目录，执行 `./unityloader unity.toml`。

手柄映射从 `SDL_GAMECONTROLLERCONFIG_FILE` 加载，未指定时尝试当前目录的
`gamecontrollerdb.txt`。本次移除了 loader 内置映射对外部条目的覆盖，部署必须
携带 [`gamecontrollerdb.txt`](../gamecontrollerdb.txt)，或通过环境变量指定设备数据库。

本游戏关键输入配置为 `touch_mode=true`、`controller_key_events=false`、
`dpad_synthesize_hat=false`、`dpad_synthesize_stick=true`。方向键和物理摇杆状态
分开保存，松开一个来源不会把另一个正在使用的来源归零。
HAT 与左摇杆合成开关独立，其他端口可以只保留 HAT。

刷新率维持既有自动探测机制，未新增或改变 FPS 配置。
`displayWidth/Height=640/480` 是已验证的逻辑视口，示例保留此设置。

## 构建

按 [`PORTING_PLAYBOOK.md`](PORTING_PLAYBOOK.md) 和 [`BUILD-DOCKER.md`](../BUILD-DOCKER.md)
使用 ARM64 构建环境。显式关闭 `JNIVM_ENABLE_RETURN_NON_ZERO`，避免缓存污染。

```sh
cmake -S . -B build-aarch64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DBD_ENABLE_LOG=ON -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF \
  -DIL2CPP_TRACE=OFF -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF
cmake --build build-aarch64 --target unityloader plugin_spacesquids -j2
aarch64-linux-gnu-strip --strip-unneeded \
  build-aarch64/unityloader build-aarch64/unityloader.d/spacesquids.so
```

通用兼容层包含 Unity 2022 启动所需的无事件 sensor/queue，以及
`swappy.disable=1`：当前 Android Choreographer 桥不能服务 Swappy 的回调线程，
需关闭该路径避免首次交换帧崩溃。游戏标题与 UI 路径保留在插件中。

## 实现要点

- **UI**：枚举可交互的 `Selectable`，过滤 inactive、CanvasRenderer cull 和
  `Graphic.Raycast`。根据所属 Canvas 的相机投影 RectTransform，将 Unity 的
  左下原点屏幕坐标转换成触摸/光标使用的左上原点坐标。
- **Combat**：最近更新的玩家 HP > 0、未暂停且没有阻挡界面时，开放原生轴输入，
  隐藏光标。HUD 的 PauseButton、射击和移动控件不算阻挡菜单。
- **Transition**：死亡等待结算、恢复倒计时等状态捕获原生输入。新模式连续三个
  更新帧稳定后生效；此前按住的键须松开，轴和扳机须回中。
- **输入归属**：observer 只写原子状态；Unity 查找、状态判断及触摸注入在 managed
  Update hook 中执行。捕获 API 在输入线程释放已交付按键并归零轴，observer
  仍可收到事件。恢复原生输入时不重放此前按住的状态。
- **触摸生命周期**：DOWN 至少跨一个 Unity 更新帧并保持 50 ms 后才允许 UP。
  不同面板取消触摸、清空边沿；同一飞船面板因卡片进入/离开视野产生的快照
  变化保留拖动。界面操作走原游戏触摸流程，不直接调用 StartGame/EndPause。
- **默认落点**：死亡弹窗避开可点击矩形并禁止初始 A 确认，消除 Restart 误触。
  设置滑块以 `Slider.handleRect` 定位，保留当前值。
- **列表滚动**：枚举全部卡片，以横坐标找相邻项；停止 ScrollRect 惯性并移动
  content 的 `anchoredPosition` 使选中项居中。原版 content 只有一张卡片宽，
  其他卡片排在矩形外，不能用 `horizontalNormalizedPosition` 表示整个列表。
- **射击**：A/RB/RT 每更新帧最多调用一次原版 `PlayerScript.Sparo()`，其内部
  `Time.time > nextfire` 控制发射，随后设置 `nextfire = Time.time + firerate`。
  默认飞船间隔 0.4 秒（约 2.5 轮/秒），11 艘飞船参数范围 0.32～0.75 秒。
  X 的按下边沿调用 `Razzo()`，弹药、冷却、HUD 和音效由原游戏管理。

该 APK 未保留 `Resources.FindObjectsOfTypeAll(Type)`，插件回退到
`Object.FindObjectsOfType(Type)`。普通状态每 8 个 Unity 更新帧刷新控件，过渡时
每帧刷新。绑定/字段布局检查失败则停用适配并记录日志。

## 容器回归

隔离副本为 `GlES_Dev:/game/SpaceSquids-cursor`，源码与构建分别位于
`/tmp/spacesquids-source`、`/tmp/spacesquids-build`。测试只使用隔离存档。
脚本 [`scripts/test-spacesquids-cursor.sh`](../scripts/test-spacesquids-cursor.sh)
使用 Xvfb `:109`、Mesa llvmpipe、dummy audio，默认 640×480；
`SCREEN_WIDTH/SCREEN_HEIGHT` 可改为 1280/720，TOML 中的逻辑尺寸也须对应调整。

```sh
# Guide → Options，A 点当前滑块，B 返回。
CASE=settings-final GAME_CONFIG=cursor-640.toml SECS=30 \
BD_PAD_REPLAY='18000:guide,22000:a,25000:b' \
bash /tmp/test-spacesquids-cursor.sh

# 主菜单左移进商店，长按左右覆盖全部飞船，B 返回。
CASE=shop-final GAME_CONFIG=cursor-640.toml SECS=42 \
BD_PAD_REPLAY='18000:dpleft,19500:a,23000:dpright:4000,29000:dpleft:2800,34000:dpright,36500:b' \
bash /tmp/test-spacesquids-cursor.sh
```

`BD_PAD_REPLAY` 为 `毫秒:按钮[:保持毫秒]`；未指定保持时间时默认 120 ms。
`BD_PAD_AXIS_REPLAY` 为 `毫秒:轴:值`，如 `23000:leftx:20000,25000:leftx:0`。
`SPACESQUIDS_TRACE=1` 开启玩家 HP、输入向量、导弹弹药和列表诊断，默认不开。
`SPACESQUIDS_DUMP=1` 打印相关游戏类布局。均不要放入正式启动脚本。

回放时间从输入线程初始化计算，场景通常约 13 秒后才可操作；负载会改变时机。
必须确认日志和截图中的动作确实生效，不能把过早丢弃的动作算通过。
截图、时间线、完整日志写入 `results/$CASE/`，每次使用新的 CASE 避免混合截图。
脚本定时 SIGINT 停止的 `exit=124` 为预期；崩溃和绑定错误会使脚本失败。
这不是原游戏正常退出/保存测试，不能据此证明退出保存路径。

| 已完成用例 | 证据 |
|---|---|
| `integration-final`、`keys-capture` | Guide、设置、商店、开局、Start、B 恢复；右摇杆光标、原生移动、开火和输入捕获 |
| `x-rocket-final` | 长按 X 只扣一轮；再次按下弹药 3→2→1→0，耗尽后 0→0 |
| `x-transition-final` | 按住 X 跨菜单、暂停、恢复不误发射，松开重按才触发 |
| `white-cursor-final` | 640×480 白色短十字，Guide 进入设置、B 返回 |
| `ui2-shop-final` | 从第 1 艘选到第 11 艘，向左返回，卡片居中 |
| `ui2-settings-final` | 100% 位置按 A 不变；按住 A 拖至左端再到右端，存档最终为 1.0 |
| `ui2-death-final` | 有足够金币的死亡弹窗中，按 A、长按 A、B 不关闭；选中金币复活及 Home，A 点击 Home 返回 |

上述最终用例没有 SEGV 或 Invalid Reference。早期诊断失败不计入通过记录。
本机截图、日志、构建缓存和二进制保存在 `build-spacesquids-container/verified/`
（Git 忽略）；游戏资源、个人存档和诊断音频均不提交。

## 掌机部署记录

2026-10-22 已部署到 `172.16.7.42:/mnt/mmc/Roms/PORTS/SpaceSquids`，
入口 `/mnt/mmc/Roms/PORTS/SpaceSquids.sh`。用户随后确认测试通过。
代码整理后的构建回归另见下方记录，未自动覆盖掌机已验收版本。

| 已验收文件 | 字节数 | SHA-256 |
|---|---:|---|
| `unityloader` | 5837128 | `84707af390aa0f66d1f1df36fcd3bad6947d0ea43bad92a7ecb132283b772dab` |
| `unityloader.d/spacesquids.so` | 274576 | `0a2de39658ae36a4dda68416f679f62d203a8884c296c5209db8e7b3857eb8b9` |

已按用户要求将原存档的 `AudioVolume_SpaceSquids` 从约 0.5 改为 `1.0`，
仅修改这一项；金币、飞船和进度未改。音效由用户确认正常，未修改音频逻辑，
原包未发现背景音乐。没有调整刷新率或普通射击/导弹冷却参数。

旧文件保存在游戏目录的 `backup-pre-cursor-20261022-01/`、
`backup-pre-x-rocket-20261022-01/`、`backup-pre-small-cursor-20261022-01/`、
`backup-pre-white-cursor-20261022-01/`、`backup-pre-ui2-20261022-01/`；
最后一个目录含旧插件及改音量前的 `playerprefs.json`。

部署遵守根目录 [`AGENTS.md`](../AGENTS.md)：先本地 staging，临时路径上传并校验
SHA-256，再备份旧文件、重命名替换。保留已有配置、资源和存档；不要启动游戏
时覆盖存档。未远程启动掌机游戏，实际操作验证由用户完成。

## 代码整理后的验证

2026-10-22 提交前使用相同 Release 开关重新构建 loader 和插件，抽出共用坐标
转换及默认光标选择逻辑，增加 API 尾部指针检查。公共输入层修复了
`dpad_synthesize_stick=false` 时连带关闭 HAT 的问题；四种开关组合及方向键
松开后保留物理摇杆输入的隔离检查均通过。

使用仓库 `unity.toml.example` 进行 640×480 容器回归：

- `cleanup-menu-shop`：Guide、当前滑块点击、B 返回、全部 11 艘卡片左右导航及居中。
- `cleanup-combat-death`：普通开火、X 弹药 3→2→1、摇杆移动/回中、Start 暂停与
  B 恢复。这轮持续存活，未将其算作死亡测试。
- `cleanup-death`：不开火进入死亡弹窗，A/长按 A/B 不关闭，方向选择金币复活
  与 Home，A 点击 Home 返回。独立 Xvfb `:119`，无崩溃或绑定错误。
- 测试脚本新增显示占用检查并验证拒绝复用正在运行的显示；`bash -n` 与
  `git diff --check` 通过。预期定时退出均为 124。

构建产物及记录保存在 `build-spacesquids-container/verified/cleanup-artifacts/`。
loader SHA-256 为 `f33c251aaf83dd60faece5555d197a8ea87feea58f9261df80a2e9aa4edb61ea`，
插件为 `44a106b5b64ebc30e1e4a1a4e793d36b55319c3b5d37c298b217ae81496f2381`。
这些是提交前容器验证产物，未覆盖上一节掌机已验收版本。

## 当前边界

- 普通控件采用几何方向评分，不循环；右摇杆自由光标及 A 拖动可作补充。
- `Graphic.Raycast` 过滤目标及父级，尚未实现通用 EventSystem 的完整顶层遮挡排序。
  新增弹窗或更换 APK 应重新验证 UI 路径及字段布局。
- 已验证商店导航和金币复活按钮的可选择性，未测试付费充值流程；购买是否成功
  依赖原游戏余额及服务。
