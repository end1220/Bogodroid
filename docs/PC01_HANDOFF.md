# PC01 移植交接（Five Hearts Under One Roof）

> 日期：2026-09-11 19:05  
> 状态：**两段 splash/logo 后白屏（进程仍活）**。`SettingData` prefs 已 HIT，但 `StartGame` 内仍 **1× NullReferenceException**；**从未**进入 `BundleManager.Initialization`。  
> 给下一任 Agent：先读本文 + [`PORTING_PLAYBOOK.md`](PORTING_PLAYBOOK.md) §4.1 + [`AGENTS.md`](../AGENTS.md)。  
> 相关 transcript：`f182ab42-e752-4eb8-9769-10f908cf1b2a`。  
> Git：**改动在分支 `pc01`（勿直接推 anbernic）**。

---

## 0. 一句话给新对话

目标：让 Boot 走到 `BundleManager.Initialization` + `injected m_URL`，再用本地 `conf/` / UnityCache 进主内容。  
当前卡点：**`StartGame` 内 NRE（与 prefs 缺 `SettingData` 无关——已种且 HIT）** → 链断，Bundle 钩子从未触发。  
禁止：void-detour 带 UniTask 返回值的方法；Firebase Create 假指针。

---

## 1. 游戏与环境

| 项 | 值 |
|----|-----|
| 包名 | `com.storytaco.pc01mclient` |
| 产品名 | Five Hearts Under One Roof（Storytaco） |
| 引擎 | Unity **2022.3.62f2** IL2CPP arm64，legacy `UnityPlayer` + `nativeRender` |
| 掌机 | `172.16.7.55`（Dropbeak `:8080`），物理内存 **972MB** |
| 远端路径 | `/mnt/mmc/Roms/PORTS/PC01/` |
| 本地 staging | `D:\Locke\gitee\LinuxArmPorts\pc01_port_stage\PC01\` |
| 源 APK 解包 | `D:\Locke\gitee\LinuxArmPorts\com.storytaco.pc01mclient\` |
| 启动 | Ports 手启 `PC01.sh`（**勿** Dropbeak-exec 游戏 `.sh`） |

诊断构建：`CMAKE_BUILD_TYPE=Release` + `BD_ENABLE_LOG=ON` + `BD_ENABLE_TRACE=ON` + **`JNIVM_ENABLE_RETURN_NON_ZERO=ON`** + strip。  
编完先 `cp unityloader unityloader.dbg` 再 strip。

掌机二进制（2026-09-11 复测白屏跑次）：

```
unityloader     e46ddee84a1e2a6549c0f3085fdb9a7770839cb8634af01345da9ae47058d0eb
pc01_cdn.so     af71803e90d4c31080f695e5efd3c2000e4aa98b329a395885b94b68416aaa30
```

---

## 2. 现状（2026-09-11 19:02 跑次，已确认）

| 阶段 | 结果 |
|------|------|
| Crash / abort | 已止住 |
| Adjust / AppLovin / FirebaseManager.Init | sdk-skip / 可忽略 |
| `InitFirebase` bypass → `StartGame(false)` | **已通** → 两段 splash/logo |
| `getString(SettingData)` | **HIT**（283B 默认 JSON 已写入 prefs） |
| `StartGame` | ENTER → HIT SettingData → **NullReferenceException** → LEAVE |
| `BundleManager.Initialization ENTER` / `injected m_URL` | **仍无** |
| 白屏后 | 空转 `eglSwapBuffers`；用户 Start+Select 正常退出 |

### 关键日志顺序（节选）

```
InitFirebase BYPASS -> StartGame(isReStart=false) ...
StartGame ENTER
getString(SettingData) HIT = "{...}" (283B)
NullReferenceException: Object reference not set to an instance of an object.
StartGame LEAVE
[BD-MEM] rss≈633MB … 爬升到 ≈651MB
```

结论：**NRE 不是「prefs 缺 key」**；反序列化之后（或并行路径上）仍有空引用。`StartGame` 同步段「返回」了，但 Boot 未进 Bundle。

### Prefs

路径：`conf/shared_prefs/com.storytaco.pc01mclient.v2.playerprefs.json`  

已种：

```json
"SettingData": "{\"MAIN_VOLUMN\":1.0,\"EFFECT_VOLUMN\":1.0,\"IS_VOICE\":true,\"IS_SUB\":true,\"DISPLAY_MODE\":0,\"BRIGHTNESS\":1.0,\"IS_FIRST_CONNECTION\":false,\"LOGIN_TYPE\":0,\"IS_PERSONAL_AD\":false,\"IS_DO_NOT_SELL\":false,\"IS_AGE_RESTRICTED_USER\":false,\"IS_CHECK_POLICY\":true,\"IS_ENABLED_PUSH_NOTIFICATION\":false}"
```

元数据字段名（注意拼写 **VOLUMN**）：`MAIN_VOLUMN` / `EFFECT_VOLUMN` / `IS_VOICE` / `IS_SUB` / `DISPLAY_MODE` / `BRIGHTNESS` / `IS_FIRST_CONNECTION` / `LOGIN_TYPE` / …  
游戏侧可见 `JsonConvert`（Newtonsoft）；枚举可能要字符串（`Google` / `Windowed`）而非 `0`——若下一刀仍卡 NRE，可试改序列化形状，但 **NRE 也可能与 SettingData 无关**（例如 `uiTitle`、DataManager 其它成员、Firebase 成功回调状态）。

---

## 3. RSS / 内存（为何 600MB+ / 1GB 机）

| 观测 | 值 |
|------|-----|
| `SystemInfo Memory` | **972MB** |
| StartGame LEAVE 后首条 `[BD-MEM]` | **rss≈633MB**，`sys_avail≈220MB` |
| 白屏空转末 | **rss≈651MB**，`sys_avail≈200MB` |
| VSZ | **≈6GB**（虚址，不是物理） |

### 为何这么高（当前判断）

1. **还没进 Bundle / UnityCache**  
   热更缓存 `conf/UnityCache/Shared/{scripts,atlas,bgm,...}` 运行时**从未打开**。这 600MB+ **不是** CDN bundle 全量进驻，而是 **splash 阶段基线**。

2. **引擎 + IL2CPP 基线就很大**  
   `so_load`：`libil2cpp.so` 映射报 **64MB**，`libunity.so` **18MB**（映射大小 ≠ 立即 RSS，但会贡献）。外加 `data.unity3d`、splash 视频/贴图、IL2CPP metadata、托管堆、GLES 资源。

3. **VSZ≈6GB 勿误读**  
   Bogodroid `so_util` 把 so 映射到高固定基址（如 `0x3600000000`）。VSZ 虚高是机制现象；**以 RSS / `sys_avail` 为准**。

4. **诊断构建偏胖**  
   当前掌机是 `BD_ENABLE_LOG`+`TRACE` ON 的诊断包；通关前可接受，上机应关日志 Release。

5. **白屏期间 RSS 仍爬升（~633→651）**  
   空转刷帧仍在分配；若长期停白屏，1GB 机 `sys_avail≈200MB` 已紧，进主内容/大 bundle 前必须先打通加载并盯 OOM。

6. **纹理拦截有限**  
   日志：`textureMaxDimETC2/ASTC … forcing 0`（压缩上传未拦）。splash 若走未拦截路径，可能偏胖；但主矛盾仍是 **Boot 未进 Bundle**，不是先做 Skul 式缩包。

**给新对话**：P0 仍是打通 Bundle；RSS 作红灯观察。进主场景后再 inventory / retier。勿因 600MB 基线直接止损（与 Skul 标题 785MB+热包不同：此处热包尚未加载）。

---

## 4. CDN / BundleManager（事实）

| 项 | 值 |
|----|-----|
| CDN | `https://d2xfbt8p71ssjd.cloudfront.net` |
| 本地槽位 | `conf/{0,1,15,16}/Android`（完整 URL） |
| 类 | `Storytaco.BundleManager`，`m_URL` offset=**72** |
| 方法 | `Initialization` **argc=3** |
| 插件 | `projects/unityloader/plugins/pc01_cdn/` → `unityloader.d/pc01_cdn.so` |
| toml | `[game_patches.pc01_cdn] enabled=true base_url=...` |
| 成功日志 | `Initialization ENTER` + `injected m_URL=...` |

硬规则：**禁止**在 `bogodroid_plugin_init` / so 刚 load 时调 `il2cpp_domain_get`；只用 `register_il2cpp_post_init`。

Boot 托管链（标题类名 **`Initialization`**）：

`InitDefaultManager` → `InitAfterManager` → `InitFirebase` → **`StartGame(bool isReStart)`** → **`InitStartGame`（argc=1，异步 `d__8`）** →（期望 Bundle…）

另有：`<InitFirebase>b__6_0`、`<StartGame>b__7_0`、`m_ActionOfInitFirebase`。

---

## 5. 已确认坑（勿重复）

| 坑 | 后果 |
|----|------|
| Firebase `Create*` dlsym 返假指针/`strdup` | `free(): invalid pointer` / double-free SIGABRT |
| void ForwardFn **detour** `InitStartGame`（UniTask） | `StartGame ENTER`→`InitStartGame LEAVE`→**SEGV**，无 splash |
| 空 hook `InitFirebase`（不调 StartGame） | UniTask/Boot 挂死或永不进游戏 |
| 只修 Adjust JNI 不跳过 Boot 门闩 | 长期黑屏；bypass→StartGame 后立刻有 splash |

### JNI / jnivm（已合入代码，在 `pc01` 分支）

- `JNIVM_ENABLE_RETURN_NON_ZERO=ON`
- 字段 `defaultVal` 解析 `L...;`；NewObject/`<init>` Instantiation
- `getClass`→`GetObjectClass`；坏 jobject 防崩
- `HookStringExtensions` 恢复 JString（`StringStubs` 曾覆盖 Instantiate→`getBytes` SEGV）
- sdk-skip：Adjust / AppLovin / MAX / Notification / firebase
- `areNotificationsEnabled` `()Z`+`()I`
- UnityPlayer `getNetworkProxySettings` / `isUaaLUseCase` STATIC+PUBLIC
- `so_resolve_link`：**仅** `SWIGRegister*` nop；勿再给 Firebase Create 假对象

### `pc01_cdn` 当前行为

- skip `FirebaseManager.*` / `AppLovinManager.Initialization`
- **detour** `Initialization.InitFirebase` → 直调 `StartGame(self, nullptr…)`（`isReStart=false`）
- **不** detour `StartGame` / `InitStartGame`（UniTask-safe）
- hook `BundleManager.Initialization`：进函数时注入 `m_URL`（尚未被调用到）

---

## 6. P0 下一步（给新对话）

1. **定位 `StartGame` 内 NRE 的真实空引用**（prefs SettingData 已排除为唯一原因）  
   - 反编译/metadata：`StartGame` / `<StartGame>b__7_0` / `InitStartGame` 用了哪些单例（`DataManager`、`uiTitle`、Firebase 状态机…）  
   - 或临时 IL2CPP 打点（**勿** void-wrap UniTask 返回值；可只在 bypass 前后 log，或 hook **无返回/已知 void** 的旁路）  
2. 若 NRE 来自「期望 Firebase 成功回调才完备的状态」：考虑调 `<InitFirebase>b__6_0`（假成功）或补齐 `m_ActionOfInitFirebase` / `m_InitState`，而不是再 void-hook `InitStartGame`。  
3. 若 Boot 仍不调 Bundle：在 **已有 BundleManager 实例** 上安全调用 `Initialization(argc=3)`（参数语义未完全查清，盲传 null 可能 SEGV）并依赖已有 `m_URL` 注入。  
4. Bundle ENTER 后：核对 `conf/{0,1,15,16}`、UnityCache、网络；再看 RSS/OOM。  
5. 次要：FMOD 输出失败、通知 stub、关日志 Release 降基线。

日志关键字：`PC01CDN`、`StartGame`、`SettingData`、`NullReference`、`Initialization ENTER`、`injected m_URL`、`BD-MEM`、`BD-SEGV`、`UnityCache`。

---

## 7. 常用命令

```powershell
# 诊断构建
docker run --rm --platform linux/amd64 `
  -v "D:\Locke\gitee\Bogodroid:/work" -w /work/build-aarch64 `
  bogo-builder:unity2017-armv7 bash -c @"
cmake . -DCMAKE_BUILD_TYPE=Release \
  -DBD_ENABLE_LOG=ON -DBD_ENABLE_TRACE=ON -DBD_ENABLE_VERBOSE=OFF \
  -DJNIVM_ENABLE_RETURN_NON_ZERO=ON
cmake --build . -j2 --target unityloader plugin_pc01_cdn
cp -f unityloader unityloader.dbg
aarch64-linux-gnu-strip --strip-unneeded unityloader
sha256sum unityloader unityloader.d/pc01_cdn.so
"@

$env:DROPBEAK_HOST = '172.16.7.55'
& 'D:\Locke\gitee\dropbeak\dist\dropbeak-cli.exe' push `
  D:\Locke\gitee\Bogodroid\build-aarch64\unityloader `
  /mnt/mmc/Roms/PORTS/PC01/unityloader --force --chunk --chunk-size 16m --verify --progress
& 'D:\Locke\gitee\dropbeak\dist\dropbeak-cli.exe' push `
  D:\Locke\gitee\Bogodroid\build-aarch64\unityloader.d\pc01_cdn.so `
  /mnt/mmc/Roms/PORTS/PC01/unityloader.d/pc01_cdn.so --force --chunk --chunk-size 4m --verify --progress

# 拉日志
$uri = 'http://172.16.7.55:8080/api/v1/files/download?path=' +
  [uri]::EscapeDataString('/mnt/mmc/Roms/PORTS/PC01/log.txt')
curl.exe -L --max-time 120 -o log.txt $uri
```

---

## 8. 对话脉络（极简）

1. 黑屏 → Missing Script → 删 `InitUnityServices`。  
2. CDN 插件过早调 IL2CPP → SEGV → `post_init`。  
3. Adjust `<init>` / `getClass` / `String.getBytes` 一系列 JNI 修复。  
4. Firebase EntryPoint → 假指针 → **SIGABRT** → 收窄仅 `SWIGRegister*`。  
5. `InitFirebase`→`StartGame` → splash+logo；void-detour `InitStartGame` → SEGV（已回滚）。  
6. 种 `SettingData` prefs → **HIT 仍 NRE** → 白屏；Bundle 未进。  
7. **交接点**：查 `StartGame` NRE 真因 / 推进到 Bundle；RSS≈650MB 为 splash 基线预警。
