# Blackmoor 2 / 黑暗荒野2 移植记录

更新日期：2026-10-29
包名：`com.fourfats.blackmoor2`（APK 内只有英文 label `Blackmoor 2`，无中文 label）
流程依据：[`PORTING_PLAYBOOK.md`](PORTING_PLAYBOOK.md) —— 这是按那条流水线走的**第二个样本**

---

## 0. 当前状态一句话

**引擎跑起来了，主菜单和剧情过场都能出图，60–90 s 不崩；手柄输入还没通。**

| 环节 | 状态 | 说明 |
|---|---|---|
| 解包落位 | ✅ | 7 个 split 全解，assets 合并后 8028 个文件 / 470 MB |
| 资产处理 | ✅ | 见 §2 的坑，必须合并所有 split |
| 构建 | ✅ | 容器内 `build-blackmoor`，增量约 45–50 s |
| 容器回放取证 | ⚠️ 部分 | 画面 ✅（主菜单/过场 PNG ~300 KB）；输入 ❌ |
| 真机确认 | ❌ 未开始 | |
| 打包发版 | ❌ 未开始 | |

---

## 1. 目标特征（决定了移植难度）

- **Unity 6000.0.70f1**，IL2CPP，arm64-v8a（版本串从 `globalgamemanagers` 内读出）
- App Bundle 7 个 APK，约 584 MB：

| APK | 大小 |
|---|---|
| `base.apk` | 50 MB |
| `split_config.arm64_v8a.apk` | 39 MB |
| `split_UnityDataAssetPack.config.astc.apk` | **469 MB**（8005 个 hash 命名 bundle） |
| `split_UnityTextureCompressionsAssetPack.config.astc.apk` | 25 MB（**只 14 个文件，但藏着引擎必需件**） |

- 原生库有加固：`libpglarmor.so` / `libnms.so` / `libbuffer_pgl.so`。
  `libil2cpp.so` 76 MB，`addr2line` 出来全是 `DllCanUnloadNow` / `CloseNLSocket` /
  `JNI_OnUnload` 之类**假符号**——别指望反汇编定位崩溃。
- 带 Firebase / Crashlytics / 穿山甲（`libtt_ugen_layout.so`），有内购。

---

## 2. 解包坑（最容易浪费半天的地方）

**`globalgamemanagers`（20 MB）、它的 4 个 `.assets.splitN`、以及 `level0`，
都在名字带 "TextureCompressions" 的那个 25 MB split 里，不在 469 MB 的 DataAssetPack 里。**

只解大的那个包 → 引擎永远报：

```text
AlertDialog: Unable to initialize the Unity Engine.
```

解包必须把所有 split 的 assets 合并成一个完整树。合并后打成 store 模式的 zip
（`zipfile.ZIP_STORED`，8028 个文件 / 470 MB），这样 `MountDataArchive` 的 zip 路线
和文件系统路线都能看到完整内容。

---

## 3. 已修的 loader 缺陷（4 处，全部在通用层，不是游戏专属）

这些缺陷**对任何 Unity 6000.0.x 端口都成立**，不是修 Blackmoor 专用补丁。

### 3.1 `initJni` 两参签名（`projects/unityloader/main.cpp`）

Unity 6000.0.x 注册的是 `initJni(Landroid/content/Context;I)V`——没有 cmdline String。
原代码只认 `(Context,int,String)V` 和 `(Context)V`，直接 fatal
`did not register it`。

### 3.2 `initJni` 的类归属是混合布局（同文件）

```
UnityPlayer                       → 有 initJni（37 个 native）
UnityPlayerForActivityOrService   → 有 nativeRender / nativeResume（41 个 native，无 initJni）
```

原代码把查找绑在"承载 nativeRender 的类"上，必然 miss。改为 `find_init_jni`：
先查 render 类，再回退到基类 `UnityPlayer`。

### 3.3 `Class.forName` 三参版本未 hook（`javastubs/javac.cpp`）

Unity 6 用 `Class.forName(String, boolean, ClassLoader)` 加载
`PlayAssetDeliveryUnityWrapper`。只 hook 了单参版本 → 返回 null jclass →
所有 PAD 调用变成 `GetStaticMethodID class is null init` → wrapper 不存在 →
libunity 空指针调用（`pc=0` at `libunity+0x7c7f08`）。

已补三参 hook，与单参**共存**：HookManager 按 lambda 推导签名注册，不同 arity 互不覆盖。

### 3.4 `getAssetPackPath` 优先真实目录（`javastubs/unity.cpp`）

`"UnityDataAssetPack"` 优先返回 `<root>/assets`，找不到才退 `.apk/assets` 形式。
`.apk/assets` 只适用于资产还在 zip 里的情况；指向不存在的 `*.apk` 会让 Unity
认为包没装，报 `Not enough storage space to install required resources.` 并卡住等点击。

### 3.5 新增 `javastubs/ump.h` + `ump.cpp`（Google UMP 同意流程）

原状：`com/google/android/ump/UserMessagingPlatform.getConsentInformation` STUB-MISS
→ 返回 null → 托管层 `NullReferenceException` → IL2CPP Threadpool 线程 SIGSEGV。
限核（`taskset` 4 核）无变化，**不是线程数竞态**。

修法要点：**Unity 的 AndroidJavaObject 桥会把返回值和参数擦成 `Ljava/lang/Object;`**：

```text
getConsentInformation  →  (Lcom/unity3d/player/UnityPlayerActivity;)Ljava/lang/Object;
setDebugGeography      →  (I)Ljava/lang/Object;
```

按 Google 文档的 Java 签名注册会一直 STUB-MISS。策略是：按擦除签名声明 C++ 方法
（返回 `std::shared_ptr<jnivm::Object>`），另留一份真实 Java 类型的重载，
descriptor 里挂同一个 Java 名，C++ 靠不同成员名区分。

`requestConsentInfoUpdate` / `loadAndShowConsentFormIfRequired` **故意不回调监听器**——
那是托管 proxy，走 proxy dispatch 太深，且离线本来无可报告。

---

## 4. 当前卡点：手柄输入

已排除的可能（都有日志证据）：

| 检查 | 结果 |
|---|---|
| Unity 是否枚举到设备 | ✅ `getDeviceIds() -> 3`，`getDevice(2) -> Xbox 360 Controller` |
| 事件是否被接受 | ✅ `nativeInjectEvent keycode=97 action=0 device=2 result=1` |
| UI 事件管道是否通 | ✅ `xdotool` 点 STORY 按钮 → 转场 → 进剧情过场 |
| 是否线程竞态 | ❌ 已排除（限核无变化） |

试过全部无效：`a`(kc=97)、`dpdown`(kc=20)、remap 成 `a="SPACE"`(kc=62)、左摇杆轴回放
（`BD_PAD_AXIS_REPLAY`）。菜单高亮一帧不动。

**注意**：`xdotool key` 发真实 X11 键盘事件时，SDL 层零条注入记录，这个测法不成立，别再用。

下一步该查的方向（按成本排序）：

1. 给 `KeyEvent` 的 getter（`getKeyCode` / `getSource` / `getMetaState` / `getScanCode` /
   `getFlags`）加一次性日志，看 Unity 到底读了什么值——最省的一步。
2. 对比能吃的端口（BrickGame / Oddmar 走同一套注入），差异在游戏侧对 device 的要求。
3. 查 `InputDevice` 的 `getSources()` / `getMotionRanges()` 是否被填全——
   InputSystem 的 gamepad device 依赖这两项判定设备形态。

---

## 5. 环境与复现

```bash
# 构建（容器 GlES_Dev 内，/workspace/Bogodroid）
docker exec GlES_Dev bash -lc 'cd /workspace/Bogodroid &&
  cmake -S . -B build-blackmoor -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBD_ENABLE_LOG=ON -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF \
    -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF &&
  cmake --build build-blackmoor -j"$(nproc)" --target unityloader'

# 回放（产物在 SEQ_DIR，日志落 GAME_DIR/log-seq.txt）
env GAME_DIR=/game/Blackmoor2 W=640 H=480 SECS=60 DUMP_AT=100 \
    SEQ_DIR=/game/Blackmoor2/seq-xxx \
    bash /workspace/Bogodroid/scripts/container-run-game-seq.sh
```

- 容器 `GlES_Dev`，32 核，`/game` 可用 888 GB
- 游戏 staging：容器 `/game/Blackmoor2`；本机 `D:/Locke/tmp/brickgame_port`
- 构建产物 168 MB **未 strip**
- 容器源码是副本，改完必须 `docker cp` 再 `touch` 才重编

**画面时序**（640×480，llvmpipe 软件 GL）：0–29 s 全黑 → 31 s 首帧 → **34 s 主菜单** →
稳定 ~300 KB/张。截图别在前 30 s 抓。

---

## 6. 未完成

- [ ] 手柄输入打通（§4）
- [ ] 进关卡实测：峰值内存、连续战斗下的稳定性
- [ ] 真机确认（掌机地址见 `PORTING_PLAYBOOK.md` §1.7.1）
- [ ] 内购相关：穿山甲 / Firebase 在断网下是否阻塞主流程
- [ ] 清理：`javastubs/binding.cpp` 里有一条临时诊断
      `[BD-PAD] registry lookup ... -> FOUND`，定位完应删
