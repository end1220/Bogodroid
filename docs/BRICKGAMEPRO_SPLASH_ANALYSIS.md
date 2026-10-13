# BrickGamePro Android Splash 缺失原因

2026-10-13。用户确认缺失的是游戏画面出现前的 Perseus Games 标志，
不是 `IntroductionMenu.ShowLogoProcess` 的方块动画。

## 结论

这是 Android Java View 显示的静态 PNG，不是视频，也不是 Unity 场景中的图像。
原资源已经部署到掌机；Bogodroid 使用 C++ JNI 对象替代 Android UnityPlayer
并直接调用原生 Unity 生命周期，没有运行 APK 的 Java 构造器及 View 布局代码，
因此没有显示这张图片。引擎初始化继续执行，原本由 splash 遮住的加载时间
在掌机上表现为空白。

## APK 与 Java 证据

分析原 `brickgamepro_421839.apk`，aapt 解析 Manifest：

- `unity.splash-enable=true`。
- `unity.splash-mode=1`。
- 图片为 `res/drawable/unity_static_splash.png`，576×1024、21,968 字节。
- 图片内容为黑底白字 Perseus Games。
- 全 APK 没有 mp4/webm/avi/mov/m4v 文件，Unity 资源没有 VideoClip/VideoPlayer。
- Unity PlayerSettings 的 `m_ShowUnitySplashScreen=false`；这是另一套引擎 splash，
  不能据此判断 Android 的静态 splash 已关闭。

jadx 对 classes.dex 的关键 Java 类反编译结果：

1. `UnityPlayerActivity.onCreate` 构造 `UnityPlayer` 并 setContentView。
2. `UnityPlayer` 构造器读 splash-enable，构造 `com.unity3d.player.l` 并 addView。
3. `l extends View` 按资源名查找 unity_static_splash，以 BitmapFactory.decodeResource
   解码；onLayout 按 splash-mode 计算等比例尺寸，设置黑底居中 BitmapDrawable。
   mode=1 对应该版本的 fit 模式。
4. UnityPlayer 渲染线程字段 `e` 初值为 5，循环中每次减一；到 0 时调度
   removeView(splash)。没有额外秒数计时或视频播放完成等待。

jadx 整包有 7 个方法反编译错误；上述构造器、View 布局和渲染线程代码均有
可读结果，未以出错方法推断 splash 逻辑。

## 加载器与掌机证据

`projects/unityloader/main.cpp` 创建的是 `javastubs/unity.h` 的 C++ UnityPlayer
对象，该类不是 Android FrameLayout；随后直接调用 initJni、nativeRecreateGfxState、
nativeResume、nativeFocusChanged 和 nativeRender。没有上述 PNG View 绘制路径。
所以问题不来自 BrickGamePro 的 Canvas 放大/裁切或按钮 alpha hook。

掌机只读检查：
`/mnt/mmc/Roms/PORTS/BrickGamePro/game/res/drawable/unity_static_splash.png`
存在，21,968 字节。该文件已在 144 文件 SHA256SUMS 清单中，不是漏推资源。
原 APK 导出图片与本地运行资源 SHA-256 相同：
`B3A1AF4D17458EB8039AEFE61C5BCB920828E9A39EE6B1585503DFC7111445A8`。

掌机本次启动日志：首次 nativeRender 前 +1634 ms，返回后 +4976 ms，
这个调用本身耗时 3342 ms；不能把全部空白时间当成独立的 splash 延时。
容器不注入按键/不启用自动游戏入口的启动回放也没有 Perseus 标志，
后续显示的是原 IntroductionMenu 方块动画。容器计时不能当作真机启动速度。

## 后续修复方向

应在 Unity 初始化期间显示原 PNG，并在正常游戏开始呈现时撤去，
不增加固定停留时间，不引入视频解码器，不改游戏内 Logo 动画。
若目标是缩短等待，需要单独分析引擎首次 nativeRender 的加载成本；
隐藏或跳过图片本身不会消除该加载成本。

本次仅检查和记录，没有修改运行代码，也没有远程启动/停止掌机游戏。
本地证据在 ignored staging `deploy/BrickGamePro/startup-validation/`：
`unity_static_splash.png`、`jadx/sources/com/unity3d/player/{UnityPlayer,l}.java`、
`current/` 启动截图/timeline/log，以及原图导出脚本。
