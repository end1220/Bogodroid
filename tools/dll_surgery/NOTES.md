# Porting .NET 9 godot mono games to 1 GB-RAM Linux handhelds

## The decision tree we should reach for FIRST next time

Before sinking days into a port, screen the game against this matrix —
we learned this the long way around with StS2 (June 2026):

1. **What runtime does the game ship?** `ls data_*/lib*.so`
   - `libcoreclr.so` only → CoreCLR self-contained. The 600 MB-class working
     set is structural and there is no quick fix.
   - `libmonosgen-2.0.so` → Mono self-contained. Already light, just port.

2. **What .NET version?** `cat data_*/*.runtimeconfig.json | grep version`
   - **.NET 9 (any minor):** Microsoft killed off
     `Microsoft.NETCore.App.Runtime.Mono.linux-arm64` at `9.0.0-preview.7`
     (Aug 2024). There is no stable 9.0.x Linux glibc MonoVM and there
     will not be one. Building it yourself from `dotnet/runtime` will
     compile but SIGSEGV at hostfxr init in untested code paths — we
     confirmed this with `v9.0.16` (May 2026). See
     [Microsoft's deprecation notice](https://learn.microsoft.com/dotnet/core/compatibility/deployment/9.0/runtimepacks).
   - .NET 8: stable Mono Linux still maintained as LTS.
   - .NET 10+: same as .NET 9 — Mono Linux gone. Android/iOS/WASM/MAUI
     are the only blessed Mono surfaces from .NET 9 onward.

3. **Can NativeAOT save us?** Grep the recovered source:
   ```
   grep -r 'Activator.CreateInstance\|Assembly.LoadFile\|Reflection.Emit' recovered/src
   ```
   - Heavy `Activator.CreateInstance(Type)` in startup paths (StS2's
     `ModelDb.Init` does this for ~3800 model types) → AOT incompatible,
     don't bother trying to publish-AOT.
   - Clean of dynamic loading → AOT is on the table and worth the work.

4. **Device RAM vs game's CoreCLR floor.** Run the game once on the
   target device, snapshot peak RSS just before OOM. If RSS-at-OOM is
   below device RAM minus the GPU budget, optimisation can fit it. If
   not, hardware is the wall.

### Hard call we hit with StS2

- Game: .NET 9.0.7 CoreCLR self-contained, reflection-heavy startup
- Device: TrimUI Smart Pro, 1 GB shared with Mali (~640 MB usable to app)
- Result: every angle hit a structural ceiling
  - MonoVM swap: 50 MB savings, not enough
  - Texture caps / mipmap off / mode=2 ASTC: marginal (textures not the
    main load)
  - PreloadManager.Enabled=false IL patch: **made it worse** (+70 MB),
    because the assets ended up loaded anyway through `Cache.LoadAsset`
    fallback, just with per-call overhead instead of batched preload
  - NativeAOT: blocked by `Activator.CreateInstance` everywhere
  - Linux MonoVM 9.0 stable: officially abandoned
- Conclusion: StS2 needs ≥ 2 GB Mali handhelds (RG556, Steam Deck, etc.).
  This game is structurally out of reach on 1 GB Mali.

The tooling we built (godot-sdl2 + DSDL2, steam_mock, godot_pck,
dll_surgery, runtime fork) is fully reusable for the **next** godot
mono port that screens "fits" through the matrix above.

---

# .NET 9 self-contained runtime layout — what each file is

When godot 4.5 mono runs a self-contained .NET 9 app, the
`data_<appname>_<rid>/` folder needs both a runtime (CoreCLR or MonoVM)
and the framework BCL DLLs alongside the game DLLs. This catalogue is
what we learned while swapping the StS2 ship-CoreCLR runtime for MonoVM
on the TrimUI Smart Pro.

The classification matters because **CoreCLR and MonoVM share many
filenames but only the host-side ones (hostfxr / hostpolicy / coreclr)
work as drop-in replacements**. Touching anything else makes the runtime
SIGSEGV before reaching managed code.

## File-by-file

### Host & runtime entry — REQUIRED, replace as a set

| File | Purpose | Notes |
|---|---|---|
| `libhostfxr.so`        | `host`/framework resolver godot finds first; reads `*.runtimeconfig.json` and picks a runtime | Single-stage entrypoint godot calls into via `hostfxr_initialize_for_runtime_config` |
| `libhostpolicy.so`     | Decides which TPA (Trusted Platform Assemblies) to feed CoreCLR | Loaded by hostfxr |
| `libcoreclr.so`        | The actual runtime. For *CoreCLR* it's the .NET CLR; for *MonoVM* it's `libmonosgen-2.0.so` renamed to `libcoreclr.so` so the same hostpolicy can load it | When swapping CoreCLR→MonoVM, this and the two above MUST come from the same source build (don't mix CoreCLR hostfxr with MonoVM libcoreclr) |

### Framework natives — REQUIRED, must match runtime

| File | Purpose | Notes |
|---|---|---|
| `libSystem.Native.so`                            | P/Invoke targets for `System.IO`, `System.Threading`, etc. — file ops, mmap, signals | Build flavour-specific. MonoVM's looks identical externally but is built against MonoVM internals |
| `libSystem.IO.Compression.Native.so`             | zlib bindings for `System.IO.Compression`                            | |
| `libSystem.Net.Security.Native.so`               | TLS/GSS-API native helpers                                            | |
| `libSystem.Security.Cryptography.Native.OpenSsl.so` | OpenSSL-backed crypto for `System.Security.Cryptography`           | |
| `libSystem.Globalization.Native.so`              | ICU bindings; replaced by a stub when `System.Globalization.Invariant=true` | |

### CoreCLR-only sub-components — REMOVE before running MonoVM

| File | Purpose | Why MonoVM doesn't need it |
|---|---|---|
| `libclrjit.so`                  | CoreCLR's tiered JIT compiler — loaded by `libcoreclr.so` on first managed method | MonoVM has its own JIT (`mono_jit_init`); leaving an old `libclrjit.so` next to a new MonoVM `libcoreclr.so` makes the runtime dlopen something with the wrong ABI → SIGSEGV before managed code runs |
| `libclrgc.so`                   | Default workstation/server GC implementation                                              | MonoVM uses SGen, embedded |
| `libclrgcexp.so`                | Experimental GC                                                                           | Same as above |
| `libcoreclrtraceptprovider.so`  | LTTng / event-pipe trace provider                                                          | MonoVM has its own tracing path |
| `libmscordbi.so`                | Managed-code debug-API surface (`ICorDebug`)                                              | Mono debug API is different |
| `libmscordaccore.so`            | Data-access companion to mscordbi (out-of-process debugging support)                     | Same |

> Convention used in repo: rename to `*.coreclr_disabled` instead of
> deleting — easy to restore when going back to CoreCLR.

### BCL DLLs (`System.Private.CoreLib.dll` + `System.*.dll` + `Microsoft.*.dll`)

These are managed assemblies, the .NET 9 framework class library. They
ARE platform-independent IL, but the runtime checks the
`System.Private.CoreLib.dll` version matches the runtime exactly, and
some BCL DLLs P/Invoke into the matching native helpers above. Swap as
a set with the runtime, or stick with the original CoreCLR set when
running CoreCLR.

### Game DLLs — keep untouched

`sts2.dll`, `GodotSharp.dll`, `Sentry.dll`, `Steamworks.NET.dll`,
`fmod.dll`-bindings (.NET side), etc. — these are the game's own code,
nothing in the runtime swap requires changing them.

The exception: we IL-patch `sts2.dll`'s `ModelDb` with
`scripts/patch_modeldb_lazy.csx` to make `Get<T>` lazy-instantiate
missing entries, which works around Mono's eager-cctor semantics. That
patch is application-level, independent of the CoreCLR↔MonoVM swap.

## Quick verification checklist

Before launching the game with a MonoVM runtime:

```
$ ls data_*_arm64/lib*.so
# should ONLY contain:
#   libcoreclr.so libhostfxr.so libhostpolicy.so   (one set, same source)
#   libSystem.Native.so libSystem.IO.Compression.Native.so
#   libSystem.Net.Security.Native.so
#   libSystem.Security.Cryptography.Native.OpenSsl.so
#   libSystem.Globalization.Native.so
# any libclr*.so or libmscor*.so → CoreCLR leftover, rename .coreclr_disabled
```

```
$ strings data_*/libcoreclr.so | grep -E '9\.0\.|monovm|mono_jit_init' | head
# expect:
#   9.0.x version
#   monovm_initialize / monovm_create_delegate
#   mono_jit_init
# if you see only coreclr_initialize/coreclr_create_delegate WITHOUT mono_*,
# this is real CoreCLR, not MonoVM
```

```
$ strings data_*/libcoreclr.so | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -V | tail
# the highest version printed must be <= device's `ldd --version`
```

```
$ cat data_*/sts2.runtimeconfig.json
# rollForward: Disable + matching version means hostfxr won't reject
# the runtime over a minor-version skew
```
