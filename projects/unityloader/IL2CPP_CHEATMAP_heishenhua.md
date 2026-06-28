# 黑神话像素版 — il2cpp 作弊字段/方法映射 (Il2CppDumper 实测)

Source: 本 APK 自带 `libil2cpp.so` + `global-metadata.dat`（metadata v29，Unity 2022）。
Il2CppDumper 跑出 `dump.cs`（48 万行，全中文，未混淆）。**RVA 不可靠（"may be protected"），
但类/方法/字段名 + 字段偏移可靠** → 运行时一律按名字解析（`il2cpp_class_get_field_from_name` /
`il2cpp_class_get_method_from_name`），偏移仅作交叉校验。

> MOD 菜单本身是 frida-il2cpp-bridge + QuickJS 字节码（见 [MOD_MENU_RE_heishenhua.md]），逻辑啃不动；
> 本表是直接 dump 游戏拿到的等价（且更干净）数据。

## 核心类 `Player_StateData : IStateData` (TypeDefIndex 2449)
玩家所有属性集中在这一个类。字段偏移（相对 `this`）：

| 作弊项 | 字段 | 偏移 | 类型 | 配套 setter (this=Player_StateData) |
|---|---|---|---|---|
| 无限血量 | `_mCurr生命` | 0x30 | float | `set__Curr生命(float)` |
| 生命上限 | `_mMax生命` | 0x28 | float | — |
| 无限法力 | `_mCurr法力` | 0x38 | float | `set__Curr法力(float)` |
| 法力上限 | `_mMax法力` | 0x34 | float | — |
| 无限气力 | `_mCurr气力` | 0x44 | float | `set__Curr气力(float)` |
| 气力上限 | `_mMax气力` | 0x3C | float | — |
| 无限豆 | `_mCurr豆子` | 0x58 | int | `set__Curr豆子(int)` |
| 无限酒 | `_mCurr酒` | 0x68 | int | `set__Curr酒(int)` |
| 酒上限 | `_mMax酒` | 0x64 | int | — |
| 葫芦数量 | `_m葫芦_数量` | 0x6C | float | — |
| 攻击力(高伤) | `_m攻击力` | 0x4C | float | (经 `F_计算加点加成` 重算) |
| 死亡标志 | `_Is死亡` | 0xFB | bool | — |
| 可受击 | `_可以受击` | 0xF5 | bool | — |

其它有用：`F_恢复生命(float)` / `F_恢复生命百分比(float)`、`_攻击力 {get;}`(读 `_m攻击力`)。
消耗入口：`PlayerController.F_喝酒/F_定身术/F_化形/F_隐身/F_精魄/F_猴子猴孙` → 最终都走对应 `set__Curr*`。

## 减伤 / 无敌 lever（已验证，最稳）
- `PlayerController : IBeAttack`（TypeDefIndex 2321）`BeAttack(AttackInfo info)` argc=1
- `AttackInfo.atk_生命` @0x10 = 承伤数值 → `×mult`（mult=0=无敌，0.2/0.5=简单/一般）。
- **现有 `[[il2cpp_patch]]` 框架已支持**（scale float field of arg-pointer），launcher「难度」已在用。
- 注：`Player_StateData` 也有自己的 `BeAttack(AttackInfo)`，但 PlayerController 那个是已验证入口。

## 高伤 lever
两条路，二选一：
1. `C_攻击判断`（TypeDefIndex 1397）`_阵营`@0x18(int 区分敌我) / `_伤害`@0x1C(float)。
   `F_对角色造成伤害(IBeAttack)` 里读 `this._伤害` → 缩放 `_伤害 ×N`，gate 玩家阵营 + save/restore
   （同一 hitbox 一次挥击命中多目标，必须 restore 防复利）。**需框架补 scale_this_field+gate+restore。**
2. 直接抬 `_m攻击力`@0x4C（更简单，但被 `F_计算加点加成` 重算，需 hook 该方法后写）。

## 资源「无限」实现（血/法力/气力/酒/豆）
消耗都经过 `set__Curr生命/法力/气力(float)` 与 `set__Curr酒/豆子(int)`，this=Player_StateData(x0 指针)。
**最省事 = hook 这些 setter，orig 后把 curr 字段强制回满**（copy `_mMax→_mCurr`，或 int 设大常量）：
- 复用现有「写 arg 指针的某偏移字段」原语，只是把 `*f *= mult` 换成 `*curr = *max`（或常量）。
- this 在 x0（整型寄存器）→ **不触发 d0 浮点参数限制**（float 入参 setter 的 value 在 d0，读不到，
  但我们不需要 value，只在 orig 后改 this 的字段即可）。
- **需框架新增一个 mode**：`freeze`（this 指针某偏移 = 另一偏移 / 常量），覆盖全部 5 个资源。

## 框架现状 vs 需扩展
现框架（main.cpp `namespace il2cpp_patch`，flat schema class/method/argc/arg/field/mult）：
- ✅ 减伤/无敌（BeAttack arg-field ×0）—— **零改动可用**
- ⬜ 资源无限 —— 需加 `freeze` mode（this 偏移 = max偏移/常量）
- ⬜ 高伤 —— 需加 `scale_this_field` + gate + restore（或 hook F_计算加点加成 抬 _m攻击力）

## Artifacts (scratchpad, 非提交)
`dumpout/dump.cs` (16MB)、`script.json`、`il2cpp.h`、`DummyDll/`。
游戏文件来源：`黑神话像素版_1.0_MOD菜单.apk` → `lib/arm64-v8a/libil2cpp.so` + `assets/bin/Data/Managed/Metadata/global-metadata.dat`。
