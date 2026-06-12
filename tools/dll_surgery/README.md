# dll_surgery — IL-level patches for shipped .NET game DLLs

When a godot 4.5 mono game ships a self-contained .NET runtime that we
can't quite reuse as-is — the runtime's Mono fallback path crashes on
the game's static initialiser ordering, or a publisher's gate flag has
to be flipped — this is where the surgical changes live. Everything
here operates on the game's existing `sts2.dll` (or equivalent) via
[Mono.Cecil](https://www.mono-project.com/docs/tools+libraries/libraries/Mono.Cecil/);
no source-level recompile, no upstream-fork maintenance.

## What's in here

```
scripts/patch_modeldb_lazy.csx     dotnet-script patcher (Mono.Cecil)
NOTES.md                           porting decision matrix + .so layout
MONOVM_SWAP.md                     CoreCLR → MonoVM swap recipe
backups/                           dll snapshots taken before patching
out/                               patched dlls (gitignored)
```

The patcher binaries (sts2.dll etc.) aren't redistributable, so they're
gitignored — pull them from the device backup you took before running
the swap.

## Why we built this

While porting **Slay the Spire 2** to a 1 GB-RAM Mali handheld
(TrimUI Smart Pro), we hit Mono's eager `beforefieldinit` semantics
clashing with StS2's `ModelDb.Init` registration order — the same code
runs cleanly on CoreCLR because CoreCLR defers cctor execution until a
static field is touched. Microsoft also officially abandoned Linux
glibc Mono at .NET 9 preview.7 (Aug 2024) — see
[`NOTES.md`](./NOTES.md) for the matrix, and the
[deprecation notice](https://learn.microsoft.com/dotnet/core/compatibility/deployment/9.0/runtimepacks).

The fix we implemented:

1. Swap the shipped `libcoreclr.so` for `Microsoft.NETCore.App.Runtime.Mono.linux-arm64` preview.7
   (the *last* officially built Linux Mono — see [`MONOVM_SWAP.md`](./MONOVM_SWAP.md))
2. IL-patch `ModelDb.Get<T>` / `Get(Type)` / `Init` so missing entries
   get lazily injected instead of throwing — see
   [`scripts/patch_modeldb_lazy.csx`](./scripts/patch_modeldb_lazy.csx)

Outcome on StS2 specifically: Mono runtime swap saved ~50 MB on the
runtime side, the IL patch resolved the `KeyNotFoundException` blocker,
but StS2's working set is still structurally above the 1 GB Mali handheld
budget. The tooling is reusable for other godot mono ports that happen
to hit the same Mono-cctor footgun.

## Using the patcher

```bash
# One-time: install dotnet-script
dotnet tool install dotnet-script -g

# Patch a game dll (read the script's header for arg semantics)
dotnet-script scripts/patch_modeldb_lazy.csx \
  -- backups/sts2.linux-9.0.7.dll \
     out/sts2.linux-9.0.7.modeldb-lazy.dll \
     /path/to/data_sts2_linuxbsd_arm64    # sibling dir for Cecil to resolve refs
```

The script's top-of-file comment block explains exactly what IL it emits
and why, so adapting it to a different game with a similar pattern is
just a matter of changing the type/method names it looks up.

## Backups

Take a backup of the game's `data_*_arm64/` folder before applying any
patch:

```bash
cp sts2.dll backups/sts2.linux-9.0.7.dll                  # original
cp -r data_*_arm64 data_*_arm64.coreclr_backup            # whole folder
```

Recovery, if anything goes wrong:

```bash
cp backups/sts2.linux-9.0.7.dll data_*_arm64/sts2.dll
cp -r data_*_arm64.coreclr_backup/* data_*_arm64/
```
