# Unity 6（6000.x）Android 端口说明

本分支（`unity6`，从 `anbernic` 拉出）把 Bogodroid 的 Android 加载器扩到 Unity **6000.x**。
只写结论与判据，通用流程见 [`PORTING_PLAYBOOK.md`](PORTING_PLAYBOOK.md)，历史案例见 [`CASE_STUDIES.md`](CASE_STUDIES.md)。

## 0. 现状

| 项 | 值 |
|----|-----|
| Unity | `6000.6.0f1`（`6000.6/respin/6000.6.0f1-fa21a270ab12`），IL2CPP，arm64-v8a，Stripping Enabled |
| 测试工程 | `C:\Users\Administrator\Desktop\Jump6\Unity6`（URP **2D** 模板；`Assets/Scenes/Stress.unity` + `Assets/Scripts/{SystemInfoDisplay,MonsterStressSpawner,SimpleCameraController,InertialRandomRotator}.cs`） |
| 构建产物 | `_Build/unity6.apk`（摊到 staging `gamefiles/unity6`，**必须与 APK 同源**，见 §0.1） |
| 验证环境 | Docker + qemu-user（`--platform linux/arm64`）+ Xvfb + Mesa llvmpipe；真机 Anbernic（Mali-G31，640x480） |
| 结果 | 引擎起、场景 `Stress` 加载、`Assembly-CSharp` 脚本执行并出画；RSS ~0.59 GB |
| 真机结果 | Anbernic H700 / Mali-G31：`scene=Stress buildIndex=0` 在 **2.27 s** 达成，5 个输入设备（含 `Xbox 360 Controller` → `XboxOneGamepadAndroid`），D-pad 轴与 `B`/`Select` 按键事件全部到达；退出为 `exited (0)` |
| 稳定性 | 容器连续 40 s 无崩溃（`timeout -s INT` 收尾，无 tombstone），帧循环持续 |
| 缺桩 | **`[STUB-MISS]` = 0**（`adbd`/`Il2Cpp` 级路径也不缺）；只剩 2 条 `[STUB-DEFAULT]`，即已由 `vm->setDefault()` 给出目标值的那两个（§1.6） |

判据（log）：

- `NativeRender returned 1, Entering loop...` 之后 `[BD-JNIBridge] ... Choreographer$FrameCallback->doFrame` 与 `@ eglSwapBuffers` 持续增长；
- 截图整屏纯色 `(41,41,41)`，正好等于 `SampleScene.unity` 里 Main Camera 的 `m_BackGroundColor: 0.16037738`（×255 ≈ 41）+ `m_ClearFlags: 2`（SolidColor）。即**引擎确实在按工程的相机设置渲染**，不是黑屏/未初始化；
- 场景与脚本（Stress 工程）：log 里出 `LOG[Unity]: [5.50] scene=Stress buildIndex=0` 与
  `LOG[Unity]: [Stress] spawned=40 prefabs=23` —— 这两句是 `SystemInfoDisplay.cs` /
  `MonsterStressSpawner.cs` 自己打的，等于**场景加载 + `Assembly-CSharp` 托管代码在跑**；
  真机（Release、无 Unity 日志转发）用同一思路：游戏脚本把证据写进
  `<端口目录>/log/unity_player.log`（`scene=`/`platform=`/`InputSystem.devices`），
  这个文件在两种构建下都存在，是跨环境可用的判据；
- 键鼠通路：容器里用 `xdotool`（XTEST）注入按键后出
  `[BD-INPUT] nativeInjectEvent (Landroid/view/InputEvent;I)Z` 与
  `[BD-INPUT] KEYDOWN scancode=4 -> KEYCODE=29`（SDL scancode → Android keycode），
  即 SDL → InputBackend → UnityPlayer → Unity native 全通。

### 0.1 症状「splash 之后黑屏、C# 脚本不执行」：先验 staging 与 APK 同源

本工程为此白跑了一整轮。现象：容器与掌机都是 **splash 能看见、随后黑屏、`SystemInfoDisplay`
不写任何日志**，看着像 Unity 6 兼容问题或 IL2CPP 没初始化。真相是 staging 里的 game data 是
**上一次 APK 构建**留下的（那次工程里还没有脚本），引擎照常跑，但元数据里没有游戏代码。

**判据（一条命令定生死）**：

```powershell
$a = (Get-Content gamefiles/unity6/assets/bin/Data/ScriptingAssemblies.json -Raw | ConvertFrom-Json).names
"staging: $($a.Count) assemblies, Assembly-CSharp=$($a -contains 'Assembly-CSharp.dll')"
```

列表里没有 `Assembly-CSharp.dll` ⇒ 数据是旧的，**不要**再去查引擎。旁证：`data.unity3d`
（本次 1.36 MB → 4.05 MB）、`global-metadata.dat`（9.15 → 9.18 MB）、`libil2cpp.so` /
`libunity.so` 的 md5 都会与 APK 里的不一致。

**重新摊包**：只覆盖 `assets/` + `lib/` + `resources.arsc` + `classes.dex`，保留目录里自建的
`unityloader` / `unity.toml` / `log.txt`；APK 内有大小写重名的 `res/` 条目会让
`ZipFile::ExtractToDirectory` 抛异常，所以先摊到临时目录再 robocopy 过去。掌机侧推完必须
`find gamedata -type f -exec md5sum {} +` 与本机逐个对齐（见 AGENTS.md 大文件推送一节）。

**当时用来否掉「引擎卡死」的证据链**：托管侧探针显示 `Time.frameCount` 与 `Time.time` 一直在涨、
`Application.isPlaying=1`，而同一次采样里 `Assembly-CSharp` 为 `nil` —— 帧循环健康，只是没有
游戏程序集。该探针（`BD_IL2CPP_PROBE`）是临时脚手架，用完即删：`il2cpp_runtime_invoke` 打在
`Time`/`SceneManager` 的静态 getter 上偶发 SEGV（`[BD-SEGV]` + libunity 地址），别带进上机版本。

## 1. Unity 6 与 2020/2022 的六处硬差异

### 1.1 玩家类布局搬家（必须处理）

Unity 6 把 `initJni` / `nativeRender` / `nativeResume` / `nativePause` / `nativeRecreateGfxState` … 从
`com/unity3d/player/UnityPlayer` **搬到** `com/unity3d/player/UnityPlayerForActivityOrService`
（GameActivity 通道则是 `UnityPlayerForGameActivity`）。这些 native 由 `libunity.so` 的 `JNI_OnLoad`
用 `RegisterNatives()` 动态注册，所以：

- 加载器必须**先**让 `FindClass()` 找得到这些类名（否则注册无处落地），
- 必须在 `JNI_OnLoad` **之后**才去 `getMethodID`（这也是 `No such Method!` 的根因：过早查询拿不到）。

`initJni` 签名也变了：`initJni(Landroid/content/Context;ILjava/lang/String;)V`，中间的 `int` 是
`com.unity3d.player.a.l` 枚举（javap 确认 `ActivityOrService=0`、`GameActivity=1`），`String` 是
Intent extra `unity` 的命令行，**不能为 null**（toml 里用 `[unity] cmdline` 或
`[package] mainIntentBundle.unity` 给，环境变量 `BD_UNITY_CMDLINE` 可临时覆盖）。

代码：`javastubs/unity.h` / `unity.cpp` / `binding.cpp`（类名注册）、`projects/unityloader/main.cpp`
（布局探测与调用）。

### 1.2 主线程 ALooper 得自己建（**必崩点**）

真机上主线程的 `ALooper` 由 ART 侧 `ActivityThread.main()` → `Looper.prepareMainLooper()` 建好；
Bogodroid 没有 ART，主线程天生没有 looper。Unity 6 在 **`libunity.so` 静态初始化**阶段就
`ALooper_forThread()` 取它，取不到只打一行

```
LOG[Unity]: Couldn't retrieve native ALooper for UI thread.
```

然后那个 null 变成 null `NdkLooper`，等到要用时才炸，且崩点离根因很远：

```
pc  0x3800f8b77c   # libunity+0xf8b77c = _ZN9NdkLooper15WaitForCreationEv 入口
lr  0x3800f8b804   # +0xf8b804 = NdkLooper::CreateHandler()+0x2c
x0  <null>         # this == nullptr，第一条指令 ldrb w8,[x0,#0xe8] 就 SEGV_MAPERR
```

修法：`main()` 一开始（仍在单线程、任何 `dlopen` 之前）自己建：

```cpp
ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);   // projects/unityloader/main.cpp
```

`thunks/ndk/alooper.c` 里已有完整实现（按线程 TLS、epoll + eventfd、`ALooper_addFd` 回调），
不需要新写；`javastubs/android_os.cpp` 的 Java `Looper` 走 `ALooper_prepare`，同一个线程会复用它。

### 1.3 Java binary name（`java.lang.Class`）必须归一化（**必崩点**）

Unity 6 到处用 **Java binary name**：`Class.forName("android.content.Context")`，并且拿这些名字去拼
`GetMethodID` / `GetFieldID` 的签名与类型（例如 `()Ljava.lang.Class;`）。`libjnivm` 的类注册表与
descriptor 全部按 **JNI internal name**（`android/content/Context`、`()Ljava/lang/Class;`）为键，于是：

- `Class.forName` 造出一个空壳幽灵类，之后在它身上的所有 `GetMethodID` 全部 miss → 返回类型默认值；
- `Object.getClass()` 因此返回 null → `DVM::FindLibrary()`
  （`...getClass().getClassLoader().findLibrary()`）失败 → `Failed to load Il2CPP`。

修复：新增 `jnivm::NormalizeDots()`（`.`→`/`，只动 `.`，保留 `[` 数组与 `$` 嵌套类），在 4 个入口点调用：

| 位置 | 作用 |
|------|------|
| `libjnivm/include/jnivm/internal/findclass.h` | `NormalizeDots()` 本体 |
| `libjnivm/src/jnivm/internal/findclass.cpp` | `InternalFindClass()` 入口归一化（也覆盖 `JNI_DEBUG` 的命名空间遍历） |
| `libjnivm/src/jnivm/internal/method.cpp` | `GetMethodID()` 签名归一化（含递归走父类时传下去的那份） |
| `libjnivm/src/jnivm/internal/field.cpp` | `GetFieldID()` 字段类型归一化 |
| `javastubs/javac.cpp` | `Class.forName` 钩子归一化后再 `vm->findClass()` |

JNI 规范本身禁止类名/签名里出现 `.`，所以对合法输入是 no-op，不会影响老的 2020/2022 端口。

判据（同一工程、同一容器，修前 → 修后）：

| 现象 | 修前 | 修后 |
|------|------|------|
| `Device Model` / `OS` | `''` / `Android OS (null) (API 0)` | `'Allwinner h700'` / `Android OS Oreo (API 26)` |
| `Activity.getPackageName` / `getFilesDir` / `getApplicationInfo` / `getPackageCodePath`、`Environment.getExternalStorageState`、`Process.setThreadPriority` | 清一色 `[STUB-MISS] … returning default` | 正常解析（`[BD-DATADIR] …packageName = com.LockeCP.Unity6`） |
| `Object.getClass()` → `DVM::FindLibrary()` | null → `Failed to load Il2CPP` / 后续 SIGSEGV | 正常 |

即：Unity 6 传进来的**签名**也带着 Java binary name，桩本身是好的、只是被点名的类型串对不上。

### 1.4 GameActivity（Android Game SDK）通道

Unity 6 默认入口是 `com.unity3d.player.UnityPlayerGameActivity extends com.google.androidgamesdk.GameActivity`，
native 侧多了 `libgame.so`（`System.loadLibrary("game")`，AGDK 的 app glue）。

本分支只把类名（`GameActivity`、`UnityPlayerGameActivity`、`UnityPlayerForRenderService`）注册进 JVM，
让 `FindClass` / `RegisterNatives` 不落空，**仍然走 ActivityOrService 通道**驱动渲染
（`libgame.so` 的 `android_native_app_glue` 生态未实现，也没必要：Bogodroid 自己就是 app glue）。
真要切 GameActivity 时注意 `initJni` 第二参传 `1`。

### 1.5 native 依赖 Java 侧生命周期时，加载器要替 Java 把动作做完（HFPStatus）

`libunity.so` 注册了两条 native 到 `com/unity3d/player/HFPStatus`：
`initHFPStatusJni()` / `deinitHFPStatusJni()`。**调用者是 Java 的 `HFPStatus.<init>`**
（dexdump：ctor 存 `Context` → `getSystemService("audio")` → `invoke-direct initHFPStatusJni`），
而 libunity 的 `initHFPStatusJni` 实现会**把 `this` 缓存起来**，之后才在音频初始化路径上对这个
缓存对象调 `clearHFPStat()` / `getHFPStat()` / `setHFPRecordingStat()`。

我们的加载器没有 Java 侧 ⇒ 缓存永远是 null，于是 libunity 拿着 null 去
`GetObjectClass(null)` → `jnivm` 把 null 类名兜成 `Invalid`：

```
[JNIVM]: FindClass Invalid
[JNIVM]: Constructed Unresolved symbol, Class=`Invalid`, Method=`clearHFPStat`, Signature=`()V`
[JNIVM]: CallMethod object is null
[JNIVM]: [STUB-MISS] Unknown Member: Class=`Invalid` Member=`clearHFPStat` Sig=`()V`
```

**判读要点**：`Class=Invalid` 不是“缺桩”，是**对象是 null**（`libjnivm` 只在 `GetObjectClass(null)` 时
造 `Invalid`）。补一个 C++ 桩类解决不了；要让 libunity 拿到真对象。

修法：`unity.h` 里建 `HFPStatus`（注册 4 个 Java 方法桩：`clearHFPStat` / `getHFPStat` /
`requestHFPStat` / `setHFPRecordingStat`，**不要**声明那两条 native，否则会盖掉 libunity 的实现），
然后在 `main.cpp` 里 `libunity` 的 `JNI_OnLoad` **之后**：

```cpp
unityPlayer->m_HFPStatus = std::make_shared<HFPStatus>(unityActivity);
hfpClass->getMethod("()V", "initHFPStatusJni")     // 这一步等价于 Java 构造函数里的那行
    .invoke(env, hfpObj.get());
```

顺序不能错：JNI_OnLoad 之前 native 还不存在，调用会落到空处、缓存依旧是 null。
修后日志变成 `Class=com/unity3d/player/HFPStatus` + `Method=clearHFPStat`（落在真对象上）。

### 1.6 代理类必须继承引擎会 forName 的**每一个**接口

`JNIBridge::newInterfaceProxy` 造出来的是一个**多继承的单一 C++ 类**（`JNIBridgeProxy`），
它必须继承所有可能被当作参数类型传进来的接口，否则 `jnivm` 解参数时 `dynamic_cast` 失败：

```
Expected N5jnivm7android4view26ViewOnLayoutChangeListenerE
[JNIVM]: Exception with Message `Invalid Reference, Unexpected Type` was thrown
```

Unity 6 会 forName 并把代理塞给 `View.addOnLayoutChangeListener` 与
`DisplayManager.registerDisplayListener`，所以这两个接口必须加进 `JNIBridgeProxy` 的基类列表
（`projects/unityloader/javastubs/jnibridge.h`），并实现其回调转发。
**注意这属于“桩被调用时抛异常”，不是 STUB-MISS**：日志里没有 `[STUB-MISS]`，只有
`Invalid Reference, Unexpected Type` + `Expected <mangled>`，容易漏掉。

同理，`vm->setDefault()` 已配置好的方法**不该**再报 `[STUB-MISS]`，`libjnivm` 现在分开打：

```
[JNIVM]: [STUB-DEFAULT] Unknown Member: Class=`android/media/AudioManager` Member=`getStreamVolume` Sig=`(I)I` -> VM::setDefault value
```

判据：`[STUB-MISS]` = 真缺桩（要补）；`[STUB-DEFAULT]` = 已按 `binding.cpp` 的 `setDefault` 给出目标值。
**排障目标是把 `[STUB-MISS]` 扫到 0**，`[STUB-DEFAULT]` 保留即可（`grep -c '\[STUB-MISS\]' log.txt`）。

## 2. 改动清单
| 文件 | 内容 |
|------|------|
| `projects/unityloader/main.cpp` | 主线程 `ALooper_prepare`；`bd_unity_cmdline()`；`bd_dump_natives()` / `bd_dump_members()` / `bd_probe_method()` 诊断（`BD_JNI_PROBE=1` 开）；Unity 6 布局探测 + `initJni(Context,int,String)`；`unityClass` 在 `JNI_OnLoad` 之后重新解析；构造 `HFPStatus` 并调 `initHFPStatusJni()`（§1.5） |
| `projects/unityloader/javastubs/unity.h` / `unity.cpp` / `binding.cpp` | Unity 6 类与 native descriptor 注册（含 `HFPStatus`、`UnityPlayerUtilities`、`PlayAssetDeliveryUnityWrapper` 的 Unity 6 重载） |
| `projects/unityloader/javastubs/jnibridge.h` / `jnibridge.cpp` | `JNIBridgeProxy` 补齐 `View$OnLayoutChangeListener` / `DisplayManager$DisplayListener` 基类（§1.6）；`disableInterfaceProxy` |
| `libjnivm/...`（见 §1.3） | `NormalizeDots` 归一化 |
| `libjnivm/src/jnivm/internal/method.cpp` | `[STUB-MISS]` 与 `[STUB-DEFAULT]` 分流（§1.6） |
| `javastubs/android.h` / `android_view.cpp` / `android_content.cpp` / `android_os.cpp` / `android_misc.cpp` | Unity 6 读到的 Java 常量/字段补齐：`ApplicationInfo.minSdkVersion`/`targetSdkVersion`、`Configuration` 全字段、`WindowManager$LayoutParams.FLAG_*`、`Sensor.TYPE_*`、`Context.SENSOR_SERVICE`/`VIBRATOR_SERVICE`、`Build.TAGS` |
| `javastubs/android_content.cpp` | `SharedPreferences` 目录用 `create_directories()` 建全路径（`android_files` 指向的目录可能还不存在，单层 `mkdir` 会 ENOENT → 存盘静默失败） |
| `javastubs/javac.cpp` | `Class.forName` 归一化 |
| `configs/unity6.toml` | 该工程的配置（铺最小项：`[unity] cmdline` 等） |

诊断开关：`BD_JNI_PROBE=1` 打印 Java 成员链（含 `<null baseclass>` 与 `[H]/[D]/[N]/[NO-HANDLE]` 标记），
用来判定“缺桩”还是“父类链接断了”；`bd_dump_natives` 在 `initJni` 前后各打一次，能把
`No such Method!` 变成一份 diff。

## 3. 已知未做 / 待办

- `[STUB-MISS]` 已清零（本工程 / 容器路径）。上真机跑具体游戏时仍会有新的一批，按 §1.6 的
  `[STUB-MISS]` vs `[STUB-DEFAULT]` 与 playbook §3 逐个补。
- `AChoreographer_*` 未提供 → Unity 回退到 **Java Choreographer**（`android/view/Choreographer` 桩），
  容器里实测能持续出帧，先不补 NDK 版。
- URP 后处理几个 `Hidden/Universal Render Pipeline/*` shader 报 “not supported or has been stripped”
  （工程侧没打进变体），后处理 pass 不执行，与加载器无关。
- 容器里 FMOD 初始化失败 → 落到 `fakemod`/SDL 音频（`[BD-AUDIO] SDL Audio device opened`）。
- **手柄已验证（真机）**：`SystemInfoDisplay.cs` 的 `log/unity_player.log` 里出现
  `device[0]=Xbox 360 Controller layout=XboxOneGamepadAndroid`，并记录到
  `Down: B`、`Axis: DpadX=1.00`、`Down: D-Right`、`Axis: DpadY=-1.00`、`Down: Select`
  —— SDL（读 `gamecontrollerdb.txt`）→ `InputBackend` → Unity Input System 全通。
  容器侧仍只有键鼠注入（没有 GameController 设备），要容器复现得挂虚拟手柄。
- 真机（Anbernic H700，Mali-G31，640x480）：上机通过 —— 场景、渲染、手柄、干净退出都验过（§0）。
  **注意 Release（`BD_ENABLE_LOG=OFF`）下 Unity 自己的 `LOG[Unity]:` 行不会进 `log.txt`**，
  真机上要看"场景有没有加载"，靠的是游戏脚本自己写的 `log/unity_player.log`
  （`SystemInfoDisplay.cs`）。`log.txt` 只剩启动头 + `exited (N)`，够看退出码。

## 4. Docker 复现（不留真机）

```powershell
# 1) 构建加载器（Debug 友好：BD_ENABLE_LOG/TRACE=ON，Release 体积）
docker run --rm -v "D:\Locke\gitee\Bogodroid:/work" -w /work/build-unity6 `
  bogo-builder:unity2017-armv7 ninja unityloader
# 每次显式带上 -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF（见 AGENTS.md；缓存陷阱）

# 2) 运行镜像（aarch64 + qemu binfmt；Xvfb + llvmpipe 顶替 Mali/EGL）
docker build --platform linux/arm64 -t bogo-arm64-test:20.04 -f <Dockerfile.test> .

# 3) 跑一局并截图
#    gamefiles/unity6  <- 摊开的 APK；-v build-unity6/unityloader:/game/unityloader
#    Xvfb :99 + DISPLAY=:99 ./unityloader ./unity6.toml > log.txt
#    import -window root shot.png   （ImageMagick；截图纯色 = 相机 clear color 即正常）
#    ⚠ Xvfb 必须等 socket 出来再启加载器：只 sleep 会在 qemu 下随机踩到
#      "SDL could not initialize! x11 not available" → unrelated-looking SIGABRT
#      （tombstone + exit 134，浪费一整轮）。见 §4.1。
#    命令：SECONDS=40 JTRACE=1 bash run.sh   （JTRACE=1 打开 JNI trace，只在排障时开）
#    判据：grep -c '\[STUB-MISS\]' log.txt  -> 0；grep -c 'Invalid Reference' -> 0

# 4) 输入通路：容器没有手柄，用 XTEST 打键盘（SDL → Unity nativeInjectEvent）
#    DISPLAY=:99 xdotool mousemove 320 240; DISPLAY=:99 xdotool key a
#    期望 log 里出现 [BD-INPUT] KEYDOWN scancode=4 -> KEYCODE=29
```

容器内没有 GPU，`SDL_VIDEODRIVER=x11` + `LIBGL_ALWAYS_SOFTWARE=1`（`BD_FAKE_EGL` 仍按默认 ON：
Unity 自己的 EGL 调用被桩掉，实际 GLES 走 SDL 创建的 context）。
`SDL_VIDEODRIVER=dummy` 跑不到窗口，只能在日志层面看启动流程，看不到帧。

排障时把 `BD_ENABLE_LOG=ON`（可加 `TRACE`/`VERBOSE`）编一版再推，通过后记得改回关日志的 Release。
`fatal_error` / SEGV 回溯**不依赖**日志开关。

### 4.1 Docker 侧的两个坑（都踩过，别再踩）

**1）Xvfb 就绪竞态 → 伪装成 Unity 崩溃**

`Xvfb :99 & sleep 2` 在 qemu 下会随负载随机失效：加载器起来时 X socket 还没建好，
`SDL_Init(SDL_INIT_VIDEO)` 报 `x11 not available`，`thunks/egl_sdl/egl_sdl.cpp` 走
`fatal_error` → `SIGABRT`。日志尾部看起来完全是 Unity 崩了（tombstone、`signal 6`、
`exit 134`、崩溃点落在刚读完的 JNI 调用后面），实际与 JNI 无关。

`tmp/unity6/run.sh` 改成轮询 socket 并保留 Xvfb 自己的 stderr：

```bash
Xvfb :99 -screen 0 640x480x24 > /game/xvfb.log 2>&1 &
for i in $(seq 1 100); do [ -e /tmp/.X11-unix/X99 ] && break; kill -0 "$XVFB" 2>/dev/null || break; sleep 0.1; done
kill -0 "$XVFB" 2>/dev/null || { echo "Xvfb died:"; cat /game/xvfb.log; exit 1; }
```

**socket 文件存在仍然不够**：后来又踩一次 —— 上面那段以 200 ms 通过，加载器依旧报
`x11 not available` 然后 `SIGABRT`。必须等**真正的 X 客户端能连上**（镜像里有 `xdpyinfo`）：

```bash
for i in $(seq 1 200); do
    [ -e /tmp/.X11-unix/X99 ] || { kill -0 "$XVFB" 2>/dev/null || break; sleep 0.1; continue; }
    xdpyinfo -display :99 >/dev/null 2>&1 && break
    sleep 0.1
done
```

**判据**：日志里出现 `SDL could not initialize! SDL_Error: x11 not available` ⇒ 先怀疑 Xvfb/显示，
不要去追 JNI。正常启动会打印 `Xvfb :99 ready (waited N00ms)`。

**2）trace 日志的量级**

`JTRACE=1`（JNI trace）下 40 s ≈ 25k 行，且 `doFrame`/`handleMessage` 会刷屏；
`run.sh` 已对这两个热点方法做了抽样打印（`hot invoke #N`）。定桩清单只 grep 两种标签：

```bash
grep -o '\[STUB-MISS\].*' log.txt | sort | uniq -c   # 真缺桩
grep -o '\[STUB-DEFAULT\].*' log.txt | sort | uniq -c # 已由 setDefault 处理
grep -c 'Invalid Reference' log.txt                   # 代理基类没继承全（§1.6）
grep -c 'CallMethod object is null' log.txt           # native 拿着 null 对象调用（§1.5）
```
