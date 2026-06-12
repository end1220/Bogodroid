# MonoVM swap for a self-contained .NET 9 godot game

**Status:** validated on TrimUI Smart Pro (glibc 2.33, Mali-G57, 1 GB RAM).
StS2 boots with MonoVM 9.0.0-preview.7, reaches main menu, atlases load,
and RSS settles ~50 MB lower than with CoreCLR. **Insufficient on this
device for StS2** — its CoreCLR-floor is too high regardless. But the
swap pipeline below is reusable for any other godot 4.5 mono game whose
runtime overhead is the actual blocker.

The whole point of doing this: godot 4.5 mono's `find_coreclr` already
falls back to MonoVM (`find_monosgen`) when no CoreCLR is present, so we
don't have to rebuild godot. We just swap files on disk.

## When to use it

You're shipping a 1 GB-RAM Linux ARM handheld (TrimUI, MiniLoong, RG353 series).
The game is godot 4.5 mono, self-contained .NET 9, RSS at OOM sits in the
500-700 MB range, and a 50-100 MB cut in the runtime overhead would make
the game fit. If the game's working set is structurally above ~700 MB
this swap won't save it (StS2 case in point).

## Microsoft's official position

For .NET 9, Microsoft retired the `Microsoft.NETCore.App.Runtime.Mono.linux-*` packages
([retired packages list](https://learn.microsoft.com/dotnet/core/compatibility/deployment/9.0/runtimepacks)).
The last published Linux glibc MonoVM build for arm64 is `9.0.0-preview.7.24405.7`
(Aug 2024). Microsoft's recommendation is to migrate to CoreCLR or stay on .NET 8.

For our purposes (handheld port), preview.7 is what we use. Building 9.0.16
stable from source is technically possible but produces a runtime that
SIGSEGVs at hostfxr init — Microsoft hasn't been validating this path for
21 months, the code path has rotted.

## The files

Drop-in replacement for the `data_<appname>_<rid>/` folder shipped with
the game. Numbers below are for StS2's `data_sts2_linuxbsd_arm64/`:

| Source for MonoVM build | Files we take |
|---|---|
| `Microsoft.NETCore.App.Runtime.Mono.linux-arm64` 9.0.0-preview.7 | `libcoreclr.so` (MonoVM, exposes `coreclr_initialize` for godot), `libhostfxr.so`, `libhostpolicy.so`, `libSystem.{Native,IO.Compression.Native,Net.Security.Native,Security.Cryptography.Native.OpenSsl,Globalization.Native}.so`, `System.Private.CoreLib.dll`, all 168 BCL DLLs the game shipped (System.*, Microsoft.*, Mono.*) |

Things you MUST remove from the CoreCLR-shipped folder before launching
with MonoVM:

```
libclrjit.so                  CoreCLR's tiered JIT, MonoVM has its own
libclrgc.so                   CoreCLR GC, MonoVM has SGen
libclrgcexp.so                experimental CoreCLR GC
libcoreclrtraceptprovider.so  CoreCLR LTTng tracing
libmscordbi.so                ICorDebug surface
libmscordaccore.so            debug data-access companion
```

Leaving these in place SIGSEGVs MonoVM during init — they sit next to
`libcoreclr.so` and get dlopen'd; the ABI doesn't match.

Don't touch: `sts2.dll`, `GodotSharp.dll`, `Sentry.dll`,
`Steamworks.NET.dll`, `0Harmony.dll`, `MonoMod.*`, `Mono.Cecil.*` — these
are the game's own managed code, runtime-agnostic.

## The runtimeconfig.json

Pin to the exact MonoVM version and disable rollForward (otherwise hostfxr
tries to match the game's nominal `version: "9.0.7"` and rejects the
preview):

```json
{
  "runtimeOptions": {
    "tfm": "net9.0",
    "rollForward": "Disable",
    "includedFrameworks": [
      { "name": "Microsoft.NETCore.App", "version": "9.0.0-preview.7.24405.7" }
    ],
    "configProperties": {
      "System.Reflection.Metadata.MetadataUpdater.IsSupported": false,
      "System.Runtime.Serialization.EnableUnsafeBinaryFormatterSerialization": false,
      "System.Globalization.Invariant": true
    }
  }
}
```

`Invariant=true` avoids pulling in ICU data; the game's `Log.Warn` will
mention "globalization-invariant mode" and that's expected.

## Build pipeline

```bash
# 1. Pull MonoVM preview.7 nupkg
curl -L \
  https://api.nuget.org/v3-flatcontainer/microsoft.netcore.app.runtime.mono.linux-arm64/9.0.0-preview.7.24405.7/microsoft.netcore.app.runtime.mono.linux-arm64.9.0.0-preview.7.24405.7.nupkg \
  -o monovm.nupkg
unzip -o monovm.nupkg -d monovm/

ART=monovm/runtimes/linux-arm64

# 2. Stage overlay — only the .so + BCL DLLs the game already ships
mkdir -p overlay
cp "$ART"/native/*.so overlay/
cp "$ART"/native/System.Private.CoreLib.dll overlay/

# 3. Filter BCL to what the game already has, never adding new DLLs
ls /path/to/game/data_*_arm64/*.dll | xargs -n1 basename > game-dlls.txt
for dll in "$ART"/lib/net9.0/*.dll; do
  grep -qx "$(basename "$dll")" game-dlls.txt && cp "$dll" overlay/
done

# 4. Apply overlay onto a clean copy of the game's data folder
cp -r /path/to/game/data_*_arm64 /tmp/staging
cp overlay/* /tmp/staging/

# 5. Disable the CoreCLR-only siblings (keep them for rollback)
cd /tmp/staging
for f in libclrgc.so libclrgcexp.so libclrjit.so \
         libcoreclrtraceptprovider.so libmscordbi.so libmscordaccore.so; do
  [ -f "$f" ] && mv "$f" "$f.coreclr_disabled"
done

# 6. Drop in the runtimeconfig.json above
# 7. Ship /tmp/staging as the new data_*_arm64
```

## Verification on the device

```
# Right runtime loaded?
strings $DATADIR/libcoreclr.so | grep -E '9\.0\.|monovm|mono_jit_init' | head
#   expected:
#     9.0.0.0
#     9.0.0-preview.7.24405.7
#     monovm_initialize / monovm_create_delegate
#     mono_jit_init

# GLIBC pinned low enough for the handheld?
strings $DATADIR/libcoreclr.so | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -V | tail
#   expected last line ≤ device's `ldd --version`

# godot finds it and the runtime initialises?
grep '.NET:' /path/to/game/log.txt | head
#   expected:
#     .NET: Initializing module...
#     Found hostfxr: ...
#     .NET: hostfxr initialized
#     .NET: GodotPlugins initialized
```

## The cctor caveat

Mono's class initialiser semantics differ from CoreCLR's
`beforefieldinit`: when `Activator.CreateInstance(typeof(T))` runs, Mono
fires T's static constructor immediately, where CoreCLR defers it. If a
game's startup path (often `ModelDb.Init` patterns or similar
reflection-driven registration) instantiates many types in a fixed order
and any of those types' cctors look up siblings via the registration
table, Mono crashes where CoreCLR doesn't.

The fix is an IL patch on the registration entry points. See
`scripts/patch_modeldb_lazy.csx` for the StS2 version (Mono.Cecil-based).
The same shape (`if (!Contains(t)) { ...Activator.CreateInstance(t)...; }`
on every lookup) ports to other games that follow the same pattern.

## Rollback

Single command on the device:

```bash
DATADIR=/path/to/game/data_*_arm64
# Restore CoreCLR-only siblings
for f in "$DATADIR"/*.coreclr_disabled; do
    mv "$f" "${f%.coreclr_disabled}"
done
# Restore game's original sts2.dll / .NET BCL from your backup
cp /backup/data_*_arm64/* "$DATADIR/"
```
