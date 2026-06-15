# IL2CPP Value-Patch Framework — Design

A **generic, config-driven** mechanism for scaling numeric values produced or
consumed by IL2CPP game methods (damage, HP, money, speed, …) from inside the
BogoDroid translation layer. The framework is universal; each game contributes
only a small TOML stanza. The launcher's "difficulty" option is just one knob on
top of this.

> First consumer: 黑悟空 (heishenhua) damage scaling. But nothing here is
> game-specific — the same loader code serves every IL2CPP port.

---

## 1. Goals / Non-goals

**Goals**
- One framework in BogoDroid; per-game knowledge is **one TOML stanza**, no code, no recompile per game.
- Resolve target methods **by name at runtime** (version-independent; survives game updates).
- **Read config once at boot.** After setup, **zero SD-card access**; hot path is pure arithmetic.
- Reusable for any numeric lever (damage out, damage in, HP, currency, …).

**Non-goals**
- Auto-detecting "what is damage" with no per-game input. Unity's engine has no
  concept of damage — it is always game logic. The irreducible per-game fact
  ("which method, which value") is supplied via config. That is the generality limit.
- A full detour library. We add the minimum inline-hook capability we need.

---

## 2. Principles

1. **Config once → RAM globals.** `wsm.toml` is already parsed once at startup
   into the global `toml::table config` (see `main.cpp` top + `config.h`). Our
   stanza rides that same read. Multipliers are copied into plain globals.
2. **Resolve once → cached raw pointers.** Method addresses resolved a single
   time, right after the IL2CPP runtime initializes. Cached as raw `uintptr_t`.
3. **Hot path = multiply + branch.** Per damage event: read one global float,
   scale one register, continue. No file I/O, no alloc, no lock, no syscall.
4. **Only the named method(s) are hooked** — never per-frame engine functions.

---

## 3. Where it plugs into the loader

`projects/unityloader/main.cpp`:
- `libil2cpp.so` is loaded at the **fixed base `0x3600000000`** (`load_so_from_file(&lil2cpp, …)`, ~line 287). No ASLR → `abs = base + RVA` (RVA only needed for discovery/debug, not for runtime resolution).
- Precedent for il2cpp-side hooking already exists: `il2cpp_log_shim_init(&lil2cpp, …)` gated on `config["debug"]["log_il2cpp"]` (~line 316).

**Timing.** `il2cpp_class_from_name` only works **after the IL2CPP runtime is
initialized** (root domain + image registration). That happens inside the game's
own startup, after `il2cpp JNI_OnLoad` / Unity player boot — *not* right after
the `.so` is loaded. Therefore:

> Hook the exported `il2cpp_init` (resolve via `so_symbol(&lil2cpp,"il2cpp_init")`).
> In our detour: call the original `il2cpp_init` first, then run patch
> resolution + installation, then return. By the time `il2cpp_init` returns,
> assemblies/classes are registered, so name resolution succeeds. One-time.

(Fallback if a target's assembly registers lazily: defer that single patch to
first-use; still one-time per method.)

---

## 4. TOML schema

```toml
# Per game. Lives in the game's *.toml (wsm.toml / hk.toml). Code-free.
[[il2cpp_patch]]
assembly = "Assembly-CSharp"        # default if omitted
namespace = ""                      # default global namespace
class  = "PlayerAttack"             # IL2CPP class declaring the method
method = "F_对范围内敌人造成伤害"     # method name (UTF-8, matches metadata)
argc   = 1                          # param count (disambiguates overloads; excludes hidden this/MethodInfo*)

# exactly one scaling target:
scale_return = { type = "f32", mult = 2.0 }       # multiply the return value
# scale_arg  = { index = 0, type = "f32", mult = 2.0 }   # or multiply an incoming arg

enabled = true
```

- `mult` is what the **launcher difficulty option** writes (stage-2 upserts it,
  same awk pattern already used for `[input.remap]`).
- `type`: `f32` / `f64` / `i32` / `i64` (selects the register class + scaling op).
- Multiple `[[il2cpp_patch]]` blocks allowed (e.g. one for outgoing damage ×2,
  one for incoming damage ×0.5).

A difficulty preset in the launcher is just a `mult` value per stanza; "原版" = `1.0`.

---

## 5. Method resolution (runtime, by name)

All APIs are exported from `libil2cpp.so` (confirmed: 235 `il2cpp_*` symbols,
incl. `il2cpp_domain_get`, `il2cpp_class_from_name`). Resolution path:

```
il2cpp_thread_attach(il2cpp_domain_get());
domain  = il2cpp_domain_get();
assemblies = il2cpp_domain_get_assemblies(domain, &count);
image   = il2cpp_assembly_get_image(<assembly matching cfg.assembly>);
klass   = il2cpp_class_from_name(image, cfg.namespace, cfg.class);
method  = il2cpp_class_get_method_from_name(klass, cfg.method, cfg.argc); // MethodInfo*
target  = method->methodPointer;   // native code address — what we hook
```

Resolve each stanza once; cache `target` + the scaling descriptor in a global
array. No RVA, no Il2CppDumper at runtime. (Il2CppDumper is used **offline, by a
human, once per game** only to *discover* the class/method/argc to write in the
TOML — see §8.)

---

## 6. Hooking primitive — extension required

**Gap:** the current `hook_address` (`loader/so_util_arm64.cpp`) is
**replace-only**. It overwrites the first instruction at the target with a `B`
into the patch arena → our function, and **does not preserve/relocate the
original instruction**, so the original body is unreachable. That suits
stub-replacement (its documented use) but our value-scaling must **call the
original** (get real damage to scale, or scale an arg then run the real body).

**Add a call-original detour primitive** (reusable loader enhancement):

```c
// Returns a callable pointer to the ORIGINAL via a trampoline, and redirects
// addr -> dst. dst can invoke *orig_out to run original behavior.
void hook_address_detour(so_module *mod, uintptr_t addr, uintptr_t dst,
                         uintptr_t *orig_out);
```

Implementation (mirrors Dobby/MinHook inline-hook, reusing arena + `arm64_encodings.h`):
1. Allocate trampoline in the patch/cave arena.
2. **Relocate** the clobbered first instruction (only 4 bytes / 1 insn are
   overwritten, since `hook_address` writes a single `B`):
   - non-PC-relative (stp/sub sp/mov/pacibsp/…): copy verbatim.
   - PC-relative (adr/adrp/b/bl/cbz/ldr-literal): fix up; if unsupported, **fail
     loudly** (don't silently miscompile). Add ADR/ADRP/B helpers to
     `arm64_encodings.h` as needed (currently only B/BR/LDR_LIT_QWORD exist).
3. Append `LDR X17,#8; BR X17; .quad (addr+4)` to resume the original body.
4. `*orig_out = trampoline`; then do the normal entry redirect to `dst`.

This is the only nontrivial reusable code; written once, every game benefits.

---

## 7. Generic scaling hook (ARM64 ABI)

IL2CPP instance method ABI: `x0 = this`, params follow in `x0..x7` (ints/ptrs)
and `d0..d7` (floats/doubles), **last arg is hidden `MethodInfo*`**. Return in
`x0` (int) or `d0` (float/double).

Two modes:

- **scale_return**: thin C detour per `type`:
  ```c
  float dmg_hook(void* self, float a /*…*/, void* mi) {   // signature per stanza
      float r = ((orig_t)g_orig[i])(self, a, mi);
      return r * g_mult[i];
  }
  ```
- **scale_arg(index,type)**: multiply the indexed arg, then tail-call original.

Because signatures vary per game, the cleanest approach is a **small generated
thunk per stanza** (we know `type`/`argc`/`index` from config at build or via a
handful of fixed templates covering the common shapes: `(this, f32) -> f32`,
`(this, f32) -> void`, `(this, i32) -> i32`, …). A generic register-shuffling
asm thunk is possible later, but fixed templates cover real games with far less
risk. The template set is engine-generic, not game-specific.

`g_mult[i]` is a `static float` set once at boot — read-only during play.

---

## 8. Per-game workflow (the one manual step)

1. Offline, once: run **Il2CppDumper** (`libil2cpp.so` + `global-metadata.dat`)
   → `dump.cs` giving class / method / argc / signature for the damage method(s).
   (`dotnet` is available locally; metadata already pulled. This is discovery
   only — never shipped, never on the hot path.)
2. Write the `[[il2cpp_patch]]` stanza(s) into the game's TOML.
3. Launcher difficulty option writes `mult`.

No loader rebuild per game.

---

## 9. Performance guarantees (explicit)

- **SD card:** touched once at boot (existing TOML read). Never again at runtime.
- **Setup:** parse config + resolve methods + install hooks = once, at
  `il2cpp_init` time.
- **Hot path (per damage event):** load one cached `float`, one FMUL, branch to
  original/return. Order of nanoseconds; fires only on damage events (≪ frame
  rate). No alloc / lock / syscall / I/O.
- Damage hook ships with **no logging** (the `il2cpp_log_shim` logging path is
  debug-only and separate).

---

## 10. Risks / open questions

- **Signature correctness** — wrong arg/return type or argc → crash. Mitigated
  by Il2CppDumper-verified config + loud failure on resolution mismatch.
- **PC-relative prologue relocation** — first instruction occasionally PC-rel;
  relocator must handle adr/adrp/b/bl or refuse. Start with verbatim + explicit
  unsupported-error; extend encoders as real cases appear.
- **Inlined damage** — if the compiler inlined the damage math into callers,
  there's no single method to hook; pick a stable boundary method instead
  (per-game discovery problem, not a framework problem).
- **Struct-by-value returns/args** (sret in x8) — out of scope for v1; templates
  cover scalar int/float only.

---

## 11. Milestones

- **M1 — Discover.** Il2CppDumper on 黑悟空 → exact class/method/argc + signature
  for outgoing-damage (and incoming-damage) methods.
- **M2 — Detour primitive.** `hook_address_detour` + minimal relocator + unit-ish
  test (hook a known method, call original, verify return passthrough).
- **M3 — POC scale.** One hardcoded stanza, `scale_return ×N`, build, deploy,
  confirm damage changes and no crash. *(gating validation)*
- **M4 — Config-drive.** Parse `[[il2cpp_patch]]`, resolve-by-name, template
  thunks, install at `il2cpp_init`.
- **M5 — Launcher UI.** Difficulty picker → stage-2 writes `mult` → tiers tuned
  (outgoing ↑ and/or incoming ↓).

Reusable outputs: the detour primitive + the patch framework live in BogoDroid;
hk and every future IL2CPP port get difficulty for the cost of one TOML stanza.
