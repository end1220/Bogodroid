# unityloader 掌机震动

2026-10-22：已实现通用的 legacy Android Vibrator → SDL joystick rumble 桥接。
2026-10-23 用户确认 Space Squids 在掌机上震动有效；容器验证和部署记录见下文。

## 支持范围

游戏原有调用决定震动时机，loader 不在标题插件里增加受击/开火判断。
Space Squids 的原生调用是 `android.os.Vibrator.hasVibrator()` 和 `vibrate(long)`，
旧日志中两者原先均落入 `[STUB-MISS]`。本次补齐：

- `Context.VIBRATOR_SERVICE = "vibrator"`；`getSystemService("vibrator")` 返回真实的兼容层对象。
- `Manifest.permission.VIBRATE = "android.permission.VIBRATE"`，沿用已有权限检查逻辑。
- `Vibrator.hasVibrator()`、`vibrate(long)`、`cancel()`。

需要 `[input] controller = true` 来初始化 SDL joystick 子系统。无需新增 TOML 配置，
支持的游戏仍使用自己的震动开关；没有可用马达时 `hasVibrator()` 返回 false，调用安静忽略。
设备输出失败后禁用该设备的震动，重新连接后才重试。
带 LOG 的诊断构建在 `[BD-HAPTICS]` 记录失败原因，关闭日志不影响震动功能。

当前仅支持 legacy 单次震动，不包含 `VibrationEffect`、波形、重复模式、
`VibratorManager` 或 `InputDevice.getVibrator()`。

## TrimUI：没有 FF 设备，走 gpio227 + motor/voltage

TrimUI（Brick / Smart Pro）的手柄不声明 `FF_RUMBLE`，`/sys/class/input/*/capabilities/ff`
为 0，因此 SDL 对任何非零 rumble 都返回 `That operation is not supported`，马达不动。
参考 `space-impact-x` 的 4e1f826 做法：固件本身由用户态驱动马达——`gpio227` 开关马达，
`/sys/class/motor/voltage` 设定强度（`500000` 关，`2000000..3500000` 档位），
MainUI 自己的震动设置写的也是这两个文件。

实现（`platform/common/haptics_backend.cpp`）：

- 首次探测同时打开 `gpio227/value` 和 `motor/voltage`；两个都能打开才算有马达，
  顺手把 `gpio227/direction` 写成 `out`，并把马达归位到关闭——上一次运行被 `-9`
  杀掉时可能正停在开启状态，没有人会替它关。
- 存在马达时**优先**于 SDL：`hasVibrator()` 返回 true（否则游戏会直接关掉震动），
  请求由输入线程每轮 `update()` 落成 `level=3500000 / gpio=1`。
- 马达没有自己的定时器，结束靠输入线程的截止检查（每轮约 4 ms）；`cancel()`、
  待机、热插拔、`shutdown()` 与快速退出都会关断。
- 短于 250 ms 的脉冲被拉长（连同截止时间一起），因为硬币马达启转有延迟，
  100 ms 的受击几乎感觉不到。
- `prepare_exit()` 同步关断马达后再等输入线程：SDL 效果由内核回收，
  sysfs 开关没人回收，进程退出前必须自己关掉。
- 日志：`[BD-HAPTICS] TrimUI motor (gpio227 + motor/voltage)`（仅带 LOG 的构建）。

上面这条只覆盖 TrimUI；有 `FF_RUMBLE` 的机型（Anbernic）仍走原来的 SDL 路径。

## 输出与生命周期

`platform/common/haptics_backend.cpp` 独立维护 SDL joystick 引用，不依赖手柄映射是否
被识别为 GameController。选择第一个支持 rumble 的设备，向强/弱两个通道发送相同的
满幅值，让单马达设备也能接收；最终震感取决于硬件驱动。

JNI/Unity 线程只更新互斥保护的最新请求，SDL 输入线程每轮处理：

- 新请求覆盖旧请求，不累积、不创建临时线程；截止时间从提交时刻计算。
- 无效时长（≤0）忽略；单次最长 60 秒，避免整型溢出和旧 SDL 的效果时长限制。
- 到期或 `cancel()` 显式输出零强度停止；已经过期的待处理请求不会延迟播放。
- 热插拔重新探测；拔出丢弃相关待处理状态，不把旧震动重播到新连接设备。
- 电源键进入待机前停止并屏蔽请求，唤醒后清空旧状态。
- 输入循环退出、Start+Select 退出时停止并释放引用。
  Unity `Activity.finish()` / `Application.Quit()` 的快速退出先请求输入线程停震，
  最多等 250 ms；若输入线程已失效，进程退出由内核回收 evdev 效果。

此处只为震动维护设备引用，不扩展现有游戏输入层的热插拔行为。

## SDL 版本与能力探测

构建 SDK 为 SDL 2.0.10，输出使用自 2.0.9 提供的 `SDL_JoystickRumble()`。
运行时存在 `SDL_JoystickHasRumble()`（2.0.18+）则动态使用它，避免引入新版硬依赖。
旧版 Linux SDL 通过 `/dev/input/event*` 的设备名称、vendor/product 和 `EVIOCGBIT(EV_FF)`
确认 `FF_RUMBLE`，只读探测，不上传效果、不播放测试脉冲，也不写死 event 节点编号。
旧版探测对仅提供 `FF_SINE` 或非 evdev 的手柄不宣称支持。

172.16.7.42 关机前调查结果：

- loader 实际加载 `/usr/lib/libSDL2-2.0.so.0`，SDL 2.0.12。
- 该 SDL 关闭 `SDL_Haptic`，但 Linux joystick rumble 路径独立于 Haptic 子系统。
- 内置 `ANBERNIC-keys` 当时位于 `/dev/input/event1`，声明 `FF_RUMBLE`，
  `EVIOCGEFFECTS` 查询可容纳 16 个效果。

已由用户确认上述掌机的实际震动输出有效，无需更换系统 SDL。
当前没有直接 evdev 输出的后备路径。

## 容器验证

Linux 构建容器，在源码根目录运行：

```sh
bash scripts/test-haptics.sh
```

测试直接编译生产后端，以模拟 SDL 和只读 evdev 接口验证新版能力 API、旧版探测、
无设备、工作线程提交、覆盖、超时、取消、无效/超长时长、待机、拔出、失败和退出清理。
测试中的 SDL 输出函数同时断言输出只发生在指定的输入线程。
`scripts/test-haptics-motor.cpp` 是第三个用例：没有手柄，用模拟的三个 sysfs 属性
（写入即替换，和 sysfs 语义一致）验证马达开关、最短脉冲、`cancel()`、待机、
`prepare_exit()` 同步关断和退出后不再输出。

JNI 集成探针只用于**无手柄的容器游戏副本**，不要打包进正式发行目录：

```sh
g++ -O2 -std=c++17 -shared -fPIC -DEnableJNIVMGC -DJNI_DEBUG \
  -Iprojects/unityloader -Iplatform/common -Ilibjnivm/include \
  scripts/test-haptics-jni-plugin.cpp -o /tmp/haptics-jni-probe.so
```

将探针临时放入隔离 IL2CPP 游戏副本的 `unityloader.d/`，用新 loader 启动。
成功时出现 `HAPTICS-TEST PASS`（探针直接写 stderr，独立于 loader 日志开关），
验证真实 JNI 字段值、服务对象类型、
`hasVibrator=false`、`vibrate(J)V` 和 `cancel()V`，同时检查日志没有上述接口的缺桩。
测试后移除探针。Space Squids 可沿用 `scripts/test-spacesquids-cursor.sh` 做启动回归。

2026-10-22 本轮验证记录：

- ARM64 Release 构建成功，LOG=ON，TRACE/VERBOSE/IL2CPP_TRACE=OFF，
  `JNIVM_ENABLE_RETURN_NON_ZERO=OFF`，产物已 strip。
- `test-haptics.sh` 的新版 SDL API / 旧版 evdev 两种路径均通过。
- Space Squids 用例 `haptics-jni-final`：运行 32 秒，JNI 探针 PASS，
  Guide 打开设置、B 返回主菜单，截图确认；预期定时停止 `exit=124`。
  无上述震动接口缺桩、SEGV 或 Invalid Reference。
- 测试探针已从游戏副本移除。产物、日志和截图保存在 Git 忽略目录
  `build-spacesquids-container/verified/haptics/`，没有部署到掌机。

2026-10-23：已将上述已验证 loader 部署到
`172.16.7.42:/mnt/mmc/Roms/PORTS/SpaceSquids/unityloader`。
文件大小 5886280 字节，上传后与替换后均校验 SHA-256：
`3e3079484dd8f370010ef9743a63cbed1c47d539ccee0a4fb36fbff70822cf49`。
旧 loader 保存在同目录 `backup-pre-haptics-20261023-01/unityloader`，
SHA-256 为 `84707af390aa0f66d1f1df36fcd3bad6947d0ea43bad92a7ecb132283b772dab`。
该轮未改插件、配置或存档，未远程启动游戏。用户随后确认震动有效。

用户反馈确认游戏实际震动有效；取消、拔出、待机与退出清理已通过模拟测试，
尚无逐项真机验收记录。

## 关闭日志的发行构建

2026-10-23 按用户要求将本次发行构建切为 `BD_ENABLE_LOG=OFF`，
TRACE/VERBOSE/IL2CPP_TRACE 同为 OFF，Release + strip。
`JNIVM_ENABLE_RETURN_NON_ZERO=OFF` 显式指定；`JNIVM_ENABLE_DEBUG` 保持 ON，
`JNIVM_ENABLE_TRACE` 由 CMake 自动跟随 LOG 关闭。
这次发行选择不改变仓库的诊断构建约定，不新增运行时日志配置。

```sh
cmake -S /tmp/spacesquids-source -B /tmp/spacesquids-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBD_ENABLE_LOG=OFF -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF \
  -DIL2CPP_TRACE=OFF -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF
cmake --build /tmp/spacesquids-build --target unityloader plugin_spacesquids -j4
strip /tmp/spacesquids-build/unityloader /tmp/spacesquids-build/unityloader.d/spacesquids.so
```

`BD_LOG`、插件 `api->log`、周期内存统计和 jnivm 常规日志在编译期关闭。
SDL/EGL、游戏引擎本身的输出及致命错误诊断不受这个开关控制；旧 `log.txt`
包含此前运行记录，不能用旧文件里的 `[BD-*]` 判断新二进制是否关闭日志。

本轮整理将剩余时长统一为毫秒向上取整，测试程序禁止 `NDEBUG`，避免断言被
发行参数移除后产生假通过。两组后端测试重新通过；无日志版本的
`haptics-nolog-final` 容器用例运行 32 秒，JNI 探针 PASS，截图确认 Guide 进入设置、
B 返回主菜单，预期定时停止 `exit=124`，新运行日志没有任何 `[BD-*]` 行。
探针已移除，验证产物保存在 `build-spacesquids-container/verified/haptics-nolog/`。

已部署到同一掌机游戏目录，上传与替换后均核对大小及 SHA-256：

| 文件 | 字节数 | SHA-256 |
|---|---:|---|
| `unityloader` | 5705976 | `c85a5528bc544126bab6e371f3857b387a1816a36507ed9cf3f3e9a343eca33b` |
| `unityloader.d/spacesquids.so` | 274576 | `44a106b5b64ebc30e1e4a1a4e793d36b55319c3b5d37c298b217ae81496f2381` |

覆盖前的带日志 loader 和原插件保存在
`/mnt/mmc/Roms/PORTS/SpaceSquids/backup-pre-haptics-nolog-20261023-01/`
（`unityloader`、`spacesquids.so`）。配置、游戏资源、映射和存档未改，
旧日志保留，未远程启动游戏。无日志构建经过容器回归，尚无用户再次真机验收记录。

## 2026-10-26 TrimUI（172.16.7.11）马达落地

TrimUI Brick 上 Space Squids 原先完全没有震动，原因在 loader 的输出路径上：

- 日志里手柄是 `device='Xbox 360 Controller' instance=0 rumble=0`，
  `/sys/class/input/*/capabilities/ff` 为 0；`hasVibrator()` 因此返回 false，
  游戏连请求都不会发，SDL 侧对 rumble 也只会回 `That operation is not supported`。
- 单独写 `/sys/class/motor/voltage` 与 `gpio227` 能让马达转（0.5 s / `3200000` 短脉冲，
  用户当场确认），说明缺的就是 loader 里的输出路径，而不是硬件或权限；
  游戏以 root 运行，`gpio227/value` 这类 root-only 属性也能写。

改动即上文 TrimUI 段落：`haptics_backend.cpp` 增加 sysfs 马达路径，探测时归位到关闭，
`scripts/test-haptics-motor.cpp` 覆盖开关、最短脉冲、`cancel()`、待机、
`prepare_exit()` 与退出后不再输出；`scripts/test-haptics.sh` 三个用例全过。

验证（先用带 LOG 的构建，`5865800` B，
`fb6ad53ff2d2442866cce6eec65ea86cecbcb4dc0fa9e3f8bc34afd9ac0b4819`）：

- 用正式入口 `/mnt/SDCARD/Roms/PORTS/SpaceSquids.sh` 远程拉起，输出重定向到
  `haptics-log.txt`；日志出现 `[BD-HAPTICS] TrimUI motor (gpio227 + motor/voltage)`
  和上面那条 `rumble=0`，确认走的是新路径。
- 用户进局确认开火/受击有震动。
- Start+Select 正常退出后 `gpio227/value=0`、`motor/voltage=500000`，没有残留震动。

最终部署的是同一份代码的 LOG 关闭构建（与上一版发行配置一致）：

| 文件 | 字节数 | SHA-256 |
|---|---:|---|
| `unityloader` | 5705976 | `a6aba4d8082989dae7042f2a3f945d8b1ee8ffc8b2fb0e52784f899501e4ee75` |

- 位置：`/mnt/SDCARD/Data/ports/SpaceSquids/unityloader`；入口、`unity.toml`、
  `unityloader.d/spacesquids.so`、游戏资源与存档均未改。
- 旧 loader（`c85a5528bc544126bab6e371f3857b387a1816a36507ed9cf3f3e9a343eca33b`，
  5705976 B）备份在 `/mnt/SDCARD/Data/ports/SpaceSquids/backup-pre-motor-20261026/`。
- 无日志构建再跑一局：`DT_NEEDED` 与上一版一致（无新增 `libav*`），游戏正常起画面；
  以 30 ms 间隔采样 `gpio227/value` 与 `motor/voltage`，抓到
  `0 500000` → `1 3500000`（约 0.4–0.5 s）→ `0 500000` 的完整脉冲，
  说明反复受击后马达都会回到静止，不会卡在开启状态。
- 测试用的 `run_with_log.sh`、采样脚本与采样结果已从掌机删除，只留备份目录。
- 同一份 loader 已放进对外发布包（`SpaceSquids-Trimui` / `SpaceSquids-Anbernic-Miniloong`
  的目录与 zip）以及 `build-spacesquids-container/release-staging/`，三处
  `SHA256SUMS` 同步为 `a6aba4d8…`，包内存档为首次启动状态；
  掌机上按该清单 `sha256sum -c` 全部 236 项通过。

