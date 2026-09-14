# Unity 6（6000.x）Android 端口说明

本分支（`unity6`，从 `anbernic` 拉出）把 Bogodroid 的 Android 加载器扩到 Unity **6000.x**。
只写结论与判据，通用流程见 [`PORTING_PLAYBOOK.md`](PORTING_PLAYBOOK.md)，历史案例见 [`CASE_STUDIES.md`](CASE_STUDIES.md)。

## 0. 现状

| 项 | 值 |
|----|-----|
| Unity | `6000.6.0f1`（`6000.6/respin/6000.6.0f1-fa21a270ab12`），IL2CPP，arm64-v8a，Stripping Enabled |
| 测试工程 | `C:\Users\Administrator\Desktop\Jump6\Unity6`（URP **2D** 模板，`Assets/Scenes/SampleScene.unity`） |
| 构建产物 | `_Build`（APK 已摊到本地 staging，见 §4） |
| 验证环境 | Docker + qemu-user（`--platform linux/arm64`）+ Xvfb + Mesa llvmpipe，**未上真机** |
| 结果 | 能启动、渲染、进入 `nativeRender` 循环；无 SIGSEGV，RSS 稳定在 ~0.45 GB |

判据（log）：

- `NativeRender returned 1, Entering loop...` 之后 `[BD-JNIBridge] ... Choreographer$FrameCallback->doFrame` 与 `@ eglSwapBuffers` 持续增长；
- 截图整屏纯色 `(41,41,41)`，正好等于 `SampleScene.unity` 里 Main Camera 的 `m_BackGroundColor: 0.16037738`（×255 ≈ 41）+ `m_ClearFlags: 2`（SolidColor）。即**引擎确实在按工程的相机设置渲染**，不是黑屏/未初始化。

## 1. Unity 6 与 2020/2022 的四处硬差异

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

修复（4 处，全部走新增的 `jnivm::NormalizeDots()`，`.`→`/`，只动 `.`，保留 `[` 数组与 `$` 嵌套类）：

| 位置 | 作用 |
|------|------|
| `libjnivm/include/jnivm/internal/findclass.h` | `NormalizeDots()` 本体 |
| `libjnivm/src/jnivm/internal/findclass.cpp` | `InternalFindClass()` 入口归一化（也覆盖 `JNI_DEBUG` 的命名空间遍历） |
| `libjnivm/src/jnivm/internal/method.cpp` | `GetMethodID()` 签名归一化（含递归走父类时传下去的那份） |
| `libjnivm/src/jnivm/internal/field.cpp` | `GetFieldID()` 字段类型归一化 |
| `javastubs/javac.cpp` | `Class.forName` 钩子归一化后再 `vm->findClass()` |

JNI 规范本身禁止类名/签名里出现 `.`，所以对合法输入是 no-op，不会影响老的 2020/2022 端口。

### 1.4 GameActivity（Android Game SDK）通道

Unity 6 默认入口是 `com.unity3d.player.UnityPlayerGameActivity extends com.google.androidgamesdk.GameActivity`，
native 侧多了 `libgame.so`（`System.loadLibrary("game")`，AGDK 的 app glue）。

本分支只把类名（`GameActivity`、`UnityPlayerGameActivity`、`UnityPlayerForRenderService`）注册进 JVM，
让 `FindClass` / `RegisterNatives` 不落空，**仍然走 ActivityOrService 通道**驱动渲染
（`libgame.so` 的 `android_native_app_glue` 生态未实现，也没必要：Bogodroid 自己就是 app glue）。
真要切 GameActivity 时注意 `initJni` 第二参传 `1`。

## 2. 改动清单

| 文件 | 内容 |
|------|------|
| `projects/unityloader/main.cpp` | 主线程 `ALooper_prepare`；`bd_unity_cmdline()`；`bd_dump_natives()` / `bd_dump_members()` / `bd_probe_method()` 诊断（`BD_JNI_PROBE=1` 开）；Unity 6 布局探测 + `initJni(Context,int,String)`；`unityClass` 在 `JNI_OnLoad` 之后重新解析 |
| `projects/unityloader/javastubs/unity.h` / `unity.cpp` / `binding.cpp` | Unity 6 类与 native descriptor 注册 |
| `libjnivm/...`（见 §1.3） | `NormalizeDots` 归一化 |
| `javastubs/javac.cpp` | `Class.forName` 归一化 |
| `configs/unity6.toml` | 该工程的配置（铺最小项：`[unity] cmdline` 等） |

诊断开关：`BD_JNI_PROBE=1` 打印 Java 成员链（含 `<null baseclass>` 与 `[H]/[D]/[N]/[NO-HANDLE]` 标记），
用来判定“缺桩”还是“父类链接断了”；`bd_dump_natives` 在 `initJni` 前后各打一次，能把
`No such Method!` 变成一份 diff。

## 3. 已知未做 / 待办

- `com.unity3d.player.UnityPlayerUtilities` 构造失败（`Failed to create java object for ...`，缺 Java 桩）；
  目前只影响它自己那条路径，不影响渲染。
- `AChoreographer_*` 未提供 → Unity 回退到 **Java Choreographer**（`android/view/Choreographer` 桩），
  容器里实测能持续出帧，先不补 NDK 版。
- 大量 `[STUB-MISS]`（`Intent.getExtras`、`Activity.getAssets`、`getObbDirs`、`getResources`、
  `PlayAssetDeliveryUnityWrapper.init` 等）按“返回类型默认值”处理，2D 模板工程不受影响；
  上真机跑具体游戏时按 playbook §3 逐个补。
- URP 后处理几个 `Hidden/Universal Render Pipeline/*` shader 报 “not supported or has been stripped”
  （工程侧没打进变体），后处理 pass 不执行，与加载器无关。
- 容器里 FMOD 初始化失败 → 落到 `fakemod`/SDL 音频（`[BD-AUDIO] SDL Audio device opened`）。
- **输入未验证**：容器内没有手柄事件源，D-pad / A-B 需真机或注入 X11 事件再测。
- 未上真机（Anbernic，Mali-G31，640x480）。

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
```

容器内没有 GPU，`SDL_VIDEODRIVER=x11` + `LIBGL_ALWAYS_SOFTWARE=1`（`BD_FAKE_EGL` 仍按默认 ON：
Unity 自己的 EGL 调用被桩掉，实际 GLES 走 SDL 创建的 context）。

排障时把 `BD_ENABLE_LOG=ON`（可加 `TRACE`/`VERBOSE`）编一版再推，通过后记得改回关日志的 Release。
`fatal_error` / SEGV 回溯**不依赖**日志开关。
