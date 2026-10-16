# BrickGamePro Android Splash 与启动视频

## 当前状态（2026-10-16）

当前使用 `assets/splashvideo.mp4` 保存的最新用户源视频，准备为静音 H.264
baseline、856×480、24 fps、8 秒、约 400 kbps 的运行文件。两台掌机统一
窗口高度 3/4 内等比居中，场景就绪即交接，不额外等待播完。播放线程
按 16 ms 总预算刷新，只转换最新到期帧；Trimui 使用已加载的原厂 EGL，
避免误载另一套驱动，独立 pbuffer 与 context 交接后释放。

运行视频 SHA-256：`b472c50e6d2554917ab9b0c4698b61bd80578236af5c1d5a9d09b54c08478bf5`。
两台掌机已部署、145 项校验通过，日志关闭。最终布局与验证位置见
[当前版本说明](BRICKGAMEPRO.md)；退出确认美术和虚线框修复见
[UI 记录](BRICKGAMEPRO_UI_ANALYSIS.md)。下列尺寸、哈希、备份与曾经未部署
的状态按各次实验时间保留，不代表当前版本。

## 2026-10-16：视频显示高度缩小 1/3

在原窗口高度 2/3 的基础上缩小 1/3，插件注册的高度比例改为 `4.0f / 9.0f`。
640×480 下视频矩形约 380×213，保持居中和原宽高比。视频文件、静音播放及
交接释放策略不变。Release/LOG=OFF 插件构建通过，容器截图确认尺寸与进入
游戏正常；12 秒测试按预定超时退出，无 `[BD-*]` 日志。
已推送至 `172.16.7.34` 的 FatFishBrick，145 个运行文件全部校验通过，存档未变。
插件 SHA-256：`8837d7a095af1038dc56216ad7e78d11b57cde30bc9f08069aa2f66eda52b2bf`。
证据与旧插件在 `deploy/BrickGamePro/splash-small-validation/`，本地部署 TAR 已更新。

## 2026-10-15：替换为从第 4 秒截取的新视频

用户指定 `newSplashVideo_from4s.mp4` 作为启动视频，源文件为桌面
`BrickGame/newSplashVideo.mp4` 第 4 秒至结尾。源文件保存为插件
`assets/splashvideo.mp4`，视频为 H.264/yuv420p、856×480、24 fps，共 96 帧。
`prepare-splash-video.py --copy-video` 移除音轨并保留编码视频数据，不再进行
有损重编码；源与运行 MP4 的视频 packet SHA-256 一致。运行视频时长恰为
4.000 秒、859946 字节，SHA-256：
`9cbd9448e448a26aa916e434ba8a5fafa22d2101c220a432ecf75ec5053fe5ac`。

保持静音、居中、高度占窗口 2/3、场景就绪即交接的既有策略。
容器启动验证：`duration=4000ms silent=1`，640×480 窗口内 viewport=570×320，
GL 无错误；GUI Start/Update 完成，视频解码器、缓冲及文件在交接时释放
（本次 93 帧），无原生崩溃。证据在
`deploy/BrickGamePro/splash-from4s-validation/`，包含旧视频备份、媒体信息、
画面数据哈希、启动日志和截图。20 秒回放的 exit=124 为测试时限。

Release loader 与插件构建通过，LOG=ON、TRACE/VERBOSE/IL2CPP_TRACE=OFF、
RETURN_NON_ZERO=OFF、strip；二进制哈希保持上一版一致，本次只更换视频资源。
本地安装包更新为 58132480 字节，145 个运行文件校验通过，SHA-256：
`b1ff10eefcec53aa3af255633aebd470262f329062d0d342faa288925f0752da`。
构建完成后，按用户后续要求已推送至 `172.16.7.34`。确认游戏退出后仅更新
视频与 SHA256SUMS，先上传 `.new` 并验证新旧哈希，最终 145 个运行文件全部
校验通过。Boy 对齐修正与单图放大插件哈希保持一致；目录无 `.new`、backup
或 `.bak` 残留。部署记录在 `splash-from4s-validation/deploy-anbernic.txt`。

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

## 最初修复方向（2026-10-13）

应在 Unity 初始化期间显示原 PNG，并在正常游戏开始呈现时撤去，
不增加固定停留时间，不引入视频解码器，不改游戏内 Logo 动画。
若目标是缩短等待，需要单独分析引擎首次 nativeRender 的加载成本；
隐藏或跳过图片本身不会消除该加载成本。

本次仅检查和记录，没有修改运行代码，也没有远程启动/停止掌机游戏。
本地证据在 ignored staging `deploy/BrickGamePro/startup-validation/`：
`unity_static_splash.png`、`jadx/sources/com/unity3d/player/{UnityPlayer,l}.java`、
`current/` 启动截图/timeline/log，以及原图导出脚本。

## 用户指定的启动动画（2026-10-14 更新）

原版 Perseus 静态图现由用户提供的 `playball.png` 替代，取代之前的三帧图。
源 PNG 保存在插件 `assets/playball.png`；转换命令和资源布局见同目录
README。原图 2880×320，横向九帧，每帧 320×320，保留用户已经完成的居中对齐，
仅将透明背景合成到黑色。每帧 250ms，一轮 2250ms，循环至首个场景准备好。
不增加固定显示时间，不改 Unity 内部 IntroductionMenu 方块动画。

容器证明 Unity 在首个 GUI Update 之前不经 loader 的 eglSwapBuffers 呈现，
仅在游戏交换缓冲时更新动画不能覆盖这一加载阶段。因此通用渲染器使用
独立 SDL GLES 上下文呈现启动动画，Unity 启动阶段的另一上下文绘制到 EGL
pbuffer。首次场景就绪后停止动画线程，将同一个游戏上下文绑定窗口，下一
游戏帧正常交换缓冲。所有游戏 GL 对象保持在其原上下文中，启动图对象在
动画上下文中清理；该路径仅由选择动画的插件启用。EGL pbuffer 不受驱动
支持时记录失败并回退到随游戏 swap 更新，此回退不保证加载期间持续播放。

此前三帧版本的容器 `animation-final/` 截图确认三帧完整与循环，记录切帧时间
1007、2018、3018、4022ms，正常交接至游戏画面。独立的退出回归运行
Guide → B 取消 → Guide → A 退出，进程 exit=0，偏好保存成功。
这些为 ARM64/QEMU + llvmpipe 容器证据；该三帧版本已部署至 Anbernic
172.16.6.250 并核对全部运行文件 SHA-256，Mali 真机播放效果仍需用户验证。

九帧版本容器证据位于 `startup-validation/playball/`：切帧时间为
267、506、754、1012、1259、1508、1765、2011ms，2272ms 回到第 0 帧。
截图确认人物完整、居中以及随后进入游戏；首个游戏 SDL swap 为启动后
4626ms。8 秒定时采集 exit=124 为预期的超时结束，不用于衡量真机启动速度。
九帧版本已推送至 Anbernic `172.16.6.250` 的
`/mnt/mmc/Roms/PORTS/BrickGamePro`，远端 `sha256sum -c SHA256SUMS`
对 146 个运行文件验证成功（exit=0）。部署未远程启动游戏，播放效果待用户手启验证。

## 启动视频（2026-10-14）

按用户最新要求，使用桌面 `splashvideo.mp4` 取代九帧图，并静音播放。
原视频 856×480、24fps、8 秒，HEVC/AAC；保存在插件 `assets/splashvideo.mp4`。
`tools/prepare-splash-video.py` 生成无音轨的 H.264/yuv420p MP4，保留分辨率和帧率，
部署到 `game/res/drawable/splashvideo.mp4`。旧帧图及 BrickGame 专属 BMP 转换脚本移除。

插件使用新增的可选 API 尾部 `set_startup_video(path, 2.0f/3.0f)`，旧 v8 字段
偏移不变。通用加载器解码视频并按时间戳推进，独立动画上下文在 Unity 初始化
期间呈现；游戏就绪后停止播放并交回窗口，不等待视频播完、不增加固定停留时间。
循环前排空 B 帧，保持整段 8 秒内容。内存仅保留当前 RGBA 图像和一个待呈现帧。
保持宽高比、黑底居中，640×480 窗口内视频为 570×320，上下各 80 像素，左右各 35 像素。

解码复用 `ffmpeg_optional`，不新增 ELF FFmpeg DT_NEEDED。当前 Anbernic 的系统
FFmpeg 运行库可用，不借用 Oddmar 目录。独立解码验证已在 ARM64 容器和
`172.16.6.250` 真机通过：384 个画面变化，8000/16000ms 循环帧与第一帧一致，exit=0。
真机只运行独立解码检查程序，未远程启动游戏。

最终容器画面验证通过：视频帧持续变化，绘制日志 `glerr=0x0`，窗口留白与上述
尺寸一致，随后正常进入游戏。视频解码器在 SDL GLES 窗口创建之后加载，以避免
容器驱动延迟加载时的静态 TLS 分配失败。验证日志记录动画交接在 +5622ms、
首个游戏 SDL swap 在 +5825ms；这些为 QEMU 时间，不能代表真机启动速度。
证据保存在 ignored staging `deploy/BrickGamePro/startup-validation/video/`。

独立退出回归在 `video/exit/`：Guide → B 取消 → Guide → A 退出，进程 exit=0，
偏好保存成功，无 SEGV。构建为 Release，LOG=ON，TRACE/VERBOSE/IL2CPP_TRACE=OFF，
显式指定 `JNIVM_ENABLE_RETURN_NON_ZERO=OFF`，部署二进制已 strip。

视频版已启用到 Anbernic `172.16.6.250` 的 `/mnt/mmc/Roms/PORTS/BrickGamePro`。
全部 145 个运行文件通过 `sha256sum -c SHA256SUMS`（exit=0）。旧加载器、插件、
清单及两张已弃用的派生 BMP 保存在同目录 `backup-startup-video-20261014-1930/`；
保留原 APK 的 `unity_static_splash.png`。未远程启动游戏，实际 Mali 窗口呈现待用户手启验证。

## 视频资源释放与格子微调（2026-10-14）

检查交接路径确认：停止动画线程并 join 后，视频状态析构释放 swscale、AVPacket、
AVFrame、AVCodecContext（含解码线程）、AVFormatContext/文件以及 RGBA vector；
启动 GL 纹理、程序、VAO 在所属上下文中删除，随后销毁 pbuffer 和独立 SDL GL 上下文。
共用 `ffmpeg_optional` 按进程缓存动态库与函数指针，不在单个视频结束时 dlclose。
因此视频播放资源及时释放，FFmpeg 动态库映射仍在；不据此保证 RSS 立即等量下降。

新增一次性析构日志。容器 `alignment-validation/after/log.txt` 中，
`video resources released after 96 frames` 在动画交接 +5606ms 和首个游戏 SDL swap
+5803ms 之前；Guide → B → Guide → A 回归 exit=0，偏好保存成功。

BrickGame 插件相机 Y 偏移 `10.60 → 10.50`，动态区域整体上移 `0.10` 格
（640×480 时约 2.112px）。底部固定黑块截图匹配的最佳位移为 -2px，
所检查的分数区域像素完全相同。证据在 ignored staging
`deploy/BrickGamePro/alignment-validation/`。本次新版已部署到 Anbernic
`172.16.6.250`，145 个运行文件哈希全部通过；旧加载器、插件与清单备份到
`backup-align-20261014-1950/`。未远程启动游戏，细微对齐效果待用户真机确认。

## Trimui 首帧停住（2026-10-16）

Trimui `172.16.7.18`（SDL mali、PowerVR Rogue GE8300）的视频解码正常，
但 SDL 的 `SDL_GL_GetProcAddress` 无法查到 EGL 入口。独立动画上下文初始化
在 `EGL entry points` 阶段失败，回退到随 Unity swap 刷新；Unity 启动期间
没有持续 swap，最终只解码/显示一帧，造成视频停在首帧。

通用 `unityloader/startup_splash.cpp` 增加 EGL 入口回退：SDL 查询失败时，
通过 `dl_iterate_phdr` 找到已经加载的宿主 `libEGL.so`，以 `RTLD_NOLOAD`
获取句柄后 `dlsym`。不按裸 soname 新加载另一套 EGL，避免 SD 卡 System/lib
中的其他 EGL 与厂商当前上下文混用；pbuffer 销毁也复用该入口。

Trimui 日志确认使用已加载的 `/usr/lib64/libEGL.so`，pbuffer 与动画线程
成功启动；同一启动过程从修复前 1 帧增至修复后 130 帧，在约 5.85 秒交接
游戏上下文，并释放视频解码器、缓冲与文件，Guide → A 正常退出。
这是日志验证，实际视觉流畅程度仍需真机观察。未变更该机插件和游戏资源。

ARM64/QEMU 容器第一次退出回归在第二次 Guide 时发生动态链接器内 SIGSEGV，
不能按 runner 的 exit=0 判定通过；保留失败证据后，同一二进制重试通过
Guide → B → Guide → A，无 crash/SEGV，视频释放与场景交接正常。
容器使用 SDL x11，其 EGL 查询直接成功，不执行本次新增的宿主 DSO 回退。
证据保存在 ignored staging `deploy/BrickGamePro/trimui-splash-validation/`。

分支核对：当前 BrickGame 已包含 oddmar 到 `484072d` 的历史，尚不包含
`origin/oddmar` 的 `68d0abc`（外部视频纹理重绑保护与 GL 删除记录清理）。
媒体 bridge 文件相同；该缺失提交与本次独立启动视频 EGL 入口失败无关，
本次未合并或 cherry-pick oddmar。最终按用户要求另构建 Release、LOG/TRACE/
VERBOSE/IL2CPP_TRACE 全 OFF、`JNIVM_ENABLE_RETURN_NON_ZERO=OFF` 的 strip 版本。

关闭日志版本 SHA-256 为
`92e16944cc7a25d6f9e83917d7a86ab3345958342d55154e1a4971aba0219ab3`，
ARM64/QEMU 的 Guide → B → Guide → A 回归 exit=0（约 36.3 秒），无 `[BD-*]`
或 crash 标记。本地 staging 与清单已更新。掌机恢复 Dropbeak 服务后，关闭
日志版已部署到 `172.16.7.18`；全部 145 个运行文件哈希通过，存档和偏好哈希
保持一致，已清理本次诊断脚本与临时校验文件。部署后未远程启动游戏。

另存桌面 `splashvideo-speed130.mp4` 供用户预览：源为桌面最新
`splashvideo.mp4`，速度 1.3 倍，时间戳除以 1.3；192 帧保留，帧率
24 → 31.2fps，时长 8 → 6.153846 秒，856×480，H.264/yuv420p，静音。
未替换游戏内视频，待用户观察效果。

## Trimui 卡顿与双机显示比例（2026-10-16）

在用户反馈卡顿后，用临时 LD_PRELOAD 计时探针测量正式 LOG OFF 加载器。
旧 31.2fps 视频、原循环在 Trimui 启动期间每秒 swap 仅 28/21/18 次；
每秒解码约 32/31/33 帧，解码耗时 232～342ms，RGBA 转换 202～287ms，
swap 本身约 30～37ms。主要问题是每轮工作结束后再固定 sleep 16ms，
叠加解码、转换和 Unity 初始化竞争后，呈现赶不上视频；降低文件码率
不能消除每个像素的颜色转换开销。

通用 unityloader 修正为每轮开始时间加 16ms 的 sleep_until，工作已经
占用预算时不再额外等待。追赶落后进度时仍顺序解码以保留依赖，仅转换
最后一个到期画面；独立 AVFrame 保存待显示画面，未来时间戳画面继续保留。
同一旧视频真机复测 swap 提升到每秒 46/43/40 次。专门的解码回归以
1ms 时间步为参考，对比 237 个不均匀 deadline，跨 19 秒和循环边界一致，
相同时间戳重复 update 不产生新画面。该调整属于 loader 的通用逻辑。

之后按用户要求切换最新桌面 `splash.mp4`，不再次加速。原文件为 HEVC/AAC，
856×480、24fps、192 帧、约 8 秒。转为静音 constrained baseline H.264，
双分片、单参考帧、无 B 帧，两遍编码 400kbps 目标（实测 396120bps），
文件 397763 字节，保持分辨率、帧率和速度，完整解码通过。
原图对比 SSIM All=0.989360。编码命令固化为插件资源工具的
`--low-cpu --preset slow`，源文件同步到插件 assets。

最终视频在 Trimui 启动期间 swap 每秒约 42/51/53 次，按约 24fps
持续解码，正常进入游戏并通过 Guide → A 退出。以上是同机计时证据，
包含不同视频、编码和初始化负载，不能把全部增益归于单一参数。
实际视觉流畅程度仍以用户真机复测为准。未修改 CPU governor。

双机大小差异来自插件版本：Trimui 旧插件 height_fraction=2/3，而
Anbernic 先前插件为 4/9。现统一为 2/3，保持原视频比例；1024×768
窗口视频约 913×512，640×480 窗口约 571×320，均居中。此选择属于
BrickGame 插件。Trimui 同步最新插件也包含此前的组合按键等修改。

最终 LOG/TRACE/VERBOSE/IL2CPP_TRACE 全 OFF、Release + strip 产物：
- loader SHA-256：`2f8fe184b1b1c531d66cb54bbda1bdd887934a92c802eeb36899df5e2aec8402`
- plugin SHA-256：`d6d09bd7b764ed85ada2053363cb891f39cc07a05363e44161e581a226e7cc57`
- video SHA-256：`8863017e8487fa3dc3360aafd7e1361c72f5abbf05e5b0736070bc2096becb9d`

已部署到 Trimui 172.16.7.18 `/mnt/SDCARD/Data/ports/FatFishBrick` 与
Anbernic 172.16.7.34 `/mnt/mmc/Roms/PORTS/FatFishBrick`，各 145 个运行
文件校验通过。未改启动脚本和存档。容器最终启动、Guide → B → Guide → A
回归 exit=0，无 crash 或 `[BD-*]` 行。临时计时探针、启动诊断脚本已从
Trimui 清理。证据位于 staging 的 `stutter/` 和 `latest-splash/`。

随后 splash 视频高度按用户要求改为窗口的 3/4。插件调用改为
`set_startup_video(..., 3.0f / 4.0f)`，宽高比和居中逻辑不变；视频和
通用 loader 未重新替换。新增退出确认装饰：桌面 `exit-game.png` 处理为
黑色线稿，白色像素透明，灰度边缘按亮度生成半透明黑色，处理后资源为
`projects/unityloader/plugins/brickgamepro/assets/exit-game.png`。
插件在 ExitMenu 上方创建非交互 RawImage，并将原确认文字和按钮直接子项
下移 58 个逻辑单位。插件 Release/LOG OFF 构建已通过容器启动退出回归，
SHA-256 为 `5f37b8031f0eddc6addd22509adf450c48334953e5d8c33d7675f0cffbaede1b`，
已部署到 Trimui `172.16.7.18` 和 Anbernic `172.16.7.34`，两台全量
145 项校验通过。

2026-10-16 退出确认布局复核：此前图片误用了 GameOver 图片的宽高比，
导致纵向压缩，原控件只下移 58 也不足以留出空间。现替换用户最新
571×464 的 exit-game.png，按亮度生成黑色半透明线稿，桌面另存
exit-game-transparent.png。图片保持 571:464，逻辑尺寸约 540×438.8，
中心 (0,260)；原文字、Yes/No 直接子项各下移 260，跳过背景
FrameButton 与新增 ExitGameIcon，逐节点记录避免重复位移。

Release / LOG、TRACE、VERBOSE、IL2CPP_TRACE 全 OFF，显式
JNIVM_ENABLE_RETURN_NON_ZERO=OFF，strip 后插件 SHA-256：
`b7f70fb2bfa4afa639066ef549cef5b85cbdc5e81e4de0f179fdb6a143368655`。
容器 Guide → B → Guide → A 回归 exit=0；已实际查看 0039.png，
图片与文字无重叠。插件部署至 Trimui 172.16.7.18 及 Anbernic
172.16.7.34，两台各 145 项完整性校验成功，仅更新插件和清单。
Trimui 1024×768 真机 framebuffer 截图 after-page0.png 已目视确认
图片比例及间距正常，B 取消、再次呼出后 A 退出完成，MainUI 恢复；
运行日志无 [BD-*]。证据留在 deploy/BrickGamePro/exit-layout-validation，
掌机上的本次截图原始文件和诊断脚本、诊断日志已清理。

2026-10-16 虚线框位置修正：上次误将 FrameButton 当作全屏背景并排除
位移，造成框留在 EXIT GAME 标题处。离线解析 level0 确认背景 Image
位于 ExitMenu 本体，FrameButton 下的 Slash、Slash (1..4) 才是 Yes/No
虚线框和中线。现 FrameButton 同原文字、按钮一起下移 260，保留原始
框与按钮的相对坐标，仅排除新增 ExitGameIcon。未更改图片尺寸或位置。
LOG OFF / Release + strip 插件 SHA-256：
`fecb2099c79a26cf9af30a08f20cfae35e22e195aa940c9facfc74feb0638022`。
容器退出回归 exit=0，截图确认；两台掌机已更新，各 145 项文件校验
通过。Trimui 真机 trimui-exit-fixed.png 确认虚线框正确包围 Yes/No，
图、标题、按钮无重叠。B 取消、A 退出回归完成，MainUI 恢复。
证据位于 deploy/BrickGamePro/exit-border-validation；远端诊断文件已清理。
