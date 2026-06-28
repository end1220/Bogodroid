# 黑神话像素版 MOD 菜单逆向 — findings

Source: `~/downloads/黑神话像素版_1.0_MOD菜单.apk` (172 MB, re-signed 2026-02-22).
Goal: 搞清这个 MOD 菜单怎么实现作弊，能否参考其原理给我们 launcher 的二级作弊页用。

## 结论 (TL;DR)
MOD 菜单 = **frida-gadget (QuickJS) + frida-il2cpp-bridge**，注入框架是「天宇 (tianyu)」。
原理与我们的 `il2cpp_patch` 框架**同源**：按 il2cpp 类名/字段名/偏移读写游戏对象。
作弊脚本被编译成 QuickJS **字节码**嵌入，菜单中文标签 + 偏移在字节码常量池里（非明文，碎片化，
不值得硬啃）。**正解 = 用 Il2CppDumper dump 游戏自带 libil2cpp.so + global-metadata.dat 拿干净字段名**
（metadata v29，已确认完全可 dump；游戏全中文，dump 极易读）。

## APK 结构 / 改包构成
- 原版文件 (日期 1980)：`lib/arm64-v8a/libil2cpp.so`、`libunity.so`、`assets/bin/Data/Managed/Metadata/global-metadata.dat`、`data.unity3d` — 未保护
- 加固壳：`assets/libjiagu*.so` + `classes.dex` 里 `com.stub.StubApp` = 360 加固
- 注入器：`classes.dex` 里 `com.tianyu.util.{DtcLoader,Configuration,a}` — 改包塞进去的
  - `DtcLoader`: `System.loadLibrary("jgdtc")` → 加载原生引擎；fallback `/data/data/com.mmCompany.heimalouxiangsutesu/lib/libjgdtc.so`
  - `a.a(String)`: XOR-16 字符串解混淆 (`ch ^ 16`)，解的只是 `android.app.ActivityThread` 这类反射类名
  - 纯引导，**dex 里没有任何作弊数据**
- 作弊引擎：`lib/arm64-v8a/libmsaoaidauth.so` (25 MB，**伪装成 OAID SDK 名防检测**，运行时即 `libjgdtc.so`)
  - 内含 frida 运行时：字符串确认 `Interceptor._attach/_replace`、`Memory._scan/_patchCode`、`frida:rpc`、`NativePointer`、`quickjs`/`qjs`
  - 即 **frida-gadget**，脚本引擎是 QuickJS
- 游戏包名：`com.mmCompany.heimalouxiangsutesu`
- 无作弊服务器 URL（配置自带，非云端下发）

## 作弊 agent 真实标识符（从 QuickJS atom 表捞出，非完整逻辑）
frida-il2cpp-bridge API：`classFromName` `classGetFieldFromName` `fieldGetOffset` `getOffset`
`classGetMethodFromName` `domainGetAssemblyFromName` 等 — 确认是开源 frida-il2cpp-bridge。

agent 自己的函数 / 它碰的游戏 il2cpp 对象：
- `applyAttackJudgeByPaths` → **攻击判断** = 我们已知的 `C_攻击判断`（伤害杠杆；增伤/减伤同源）
- `GameManager` + `_instance`
- `UIManager`
- `heishenhua_xiangsu_mmger`（总管理器类，疑似 = 各种资源/状态入口）
- `CameraController` + `ZoomSpeed`
- `_trackingEnemies` `setEnemyMapData` `setBossMapData` `AreaMapData` `_mMapID` `_StartPos_Model`
  `GenGzMonster` `GetBossFromGenerator` `LoadBossGeneratorByMapId`（地图 / Boss / 传送 / 锁敌）
- `SkipPlayer` `skipDialog`（跳过）

> 注意：捞出的 headline 功能偏「地图/Boss/传送/锁敌/跳过 + 攻击判断」。用户菜单里报的
> 无敌/无限血/法力/气力/酒/豆 这些资源类作弊，其 il2cpp 字段名是中文，被压在字节码里没干净漏出来。
> → 这些字段直接走 Il2CppDumper 找最快（见下）。

## 我们这边怎么做（参考但不照搬）
- **不照搬**它的引擎：frida-gadget + 悬浮窗 + 加固对我们 launcher 无用且搬不动（我们跑原版 + unityloader，不是改包）。
- **参考其原理**：按 il2cpp 名字/偏移读写字段 — 我们 `il2cpp_patch` 框架已经是这个原理。
- **数据来源**：Il2CppDumper dump 本 APK 的 libil2cpp.so + global-metadata.dat（v29，可 dump）→ 找
  - 无敌/无限血：承伤 ×0（= 现「难度·无敌」，`PlayerController::BeAttack` arg0 field 0x10 `atk_生命`，已验证）
  - 高伤害：`C_攻击判断::F_对角色造成伤害` scale `_伤害`@0x1C + 阵营 gate + restore（需框架补 scale_this_field 模式）
  - 无限酒/法力/气力/豆：找各自「消耗方法(float amount)」×0，多数现有 ×0 模式可表达
- 框架现状 (main.cpp `namespace il2cpp_patch`)：仅支持「把某 arg 的 float 字段 ×= mult」(flat schema: class/method/argc/arg/field/mult)。
  高伤害的 gate+restore 需扩展。
