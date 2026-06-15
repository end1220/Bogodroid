# M1 findings — 黑悟空 (heishenhua) damage map

Source: Il2CppDumper on `libil2cpp.so` (metadata v29, Unity 2022) + `global-metadata.dat`.
Dump is reproducible offline; nothing here ships. Resolution at runtime is **by name**
(no RVA), so these names — not addresses — are what the framework needs.

> Il2CppDumper warned "This file may be protected" → method **RVAs are not reliable**
> in the dump, which is *fine*: we resolve by name via `il2cpp_class_get_method_from_name`.
> Names/signatures/field-offsets below are reliable.

## The damage data model

All damageable entities implement one interface:

```csharp
interface IBeAttack { void BeAttack(AttackInfo attackInfo); }   // enemies, player, NPCs, breakables
```

Damage travels inside `AttackInfo` (TypeDefIndex 2196), a **reference type**:

| field | offset | meaning |
|---|---|---|
| `float atk_生命` | **0x10** | **HP damage ← the number we scale** |
| `float atk_僵直` | 0x14 | stagger |
| `float atk_阴阳` | 0x18 | yin-yang dmg |
| `float atk_击退力度` | 0x1C | knockback |

The generic attack hitbox `C_攻击判断` (used by BOTH player & enemy attacks) carries:

| field | offset | meaning |
|---|---|---|
| `int   _阵营`  | **0x18** | **faction** — distinguishes player vs enemy attacker |
| `float _伤害`  | **0x1C** | this attack's damage |
| `float _阴阳伤害` | 0x20 | |
| `float _击退力度` | 0x28 | |

`C_攻击判断.F_对角色造成伤害(IBeAttack Character)` = the single point where an attack
applies to a target (reads `this._伤害`, builds an `AttackInfo`, calls `target.BeAttack`).
`F_对范围人员造成伤害()` loops it over targets in range.

## Two clean levers (both single-method hooks)

### A. 减伤 / take less damage  — RECOMMENDED first (simplest, zero ambiguity)
- **Hook:** `PlayerController::BeAttack(AttackInfo info)`  (argc=1)
- **Action:** `info->atk_生命 (float @0x10) *= mult_in;` then call original.
- Single class. `AttackInfo` is built fresh per hit → **no compounding**, no faction
  check (it is literally the player receiving damage). `mult_in < 1` = easier.

### B. 增伤 / deal more damage
- **Hook:** `C_攻击判断::F_对角色造成伤害(IBeAttack Character)` (argc=1)
- **Action (detour, save/restore to avoid compounding across multi-target swings):**
  ```
  if (this->_阵营 == <player faction>) {
      float save = this->_伤害(@0x1C);
      this->_伤害 = save * mult_out;
      orig(this, Character, mi);
      this->_伤害 = save;          // restore — same hitbox hits several targets per swing
  } else orig(this, Character, mi);
  ```
- `_阵营` player value: TBD at M3 (read it live, or try both). One method covers all
  player attacks; enemy attacks pass through untouched.

> Lever A needs only "scale a float field of a pointer-arg, then call original".
> Lever B needs "read/scale/restore a float field of `this`, around original".
> Both require the **call-original detour** (`hook_address_detour`, see design §6).

## Proposed TOML (per design §4, extended with field/faction scaling)

```toml
[[il2cpp_patch]]                 # 减伤
class="PlayerController" method="BeAttack" argc=1
scale_arg_field={ arg=0, offset=0x10, type="f32", mult=0.5 }   # info.atk_生命

[[il2cpp_patch]]                 # 增伤
class="C_攻击判断" method="F_对角色造成伤害" argc=1
scale_this_field={ offset=0x1C, type="f32", mult=2.0,
                   gate_field={ offset=0x18, type="i32", equals=<player_阵营> },
                   restore=true }                              # this._伤害
```

This extends the framework's scaling modes from §7: `scale_return`, `scale_arg`,
**`scale_arg_field`**, **`scale_this_field` (+ optional gate + restore)**. Still
generic — any game expresses its damage path with these primitives.

## Artifacts (local, /tmp, not committed)
- `/tmp/libil2cpp.so`, `/tmp/gm.dat` (pulled from device)
- `/tmp/dumpout/dump.cs` (16 MB), `script.json`, `il2cpp.h`
- Il2CppDumper built at `/tmp/Il2CppDumper` (net8.0)
