# StS2 patches

Game-specific patches applied to a Slay the Spire 2 recovered project tree
before repack. `apply.sh` runs between `reimport.sh` and `repack.sh` in the
godot_pck pipeline; `build.sh` is the one-shot orchestrator that runs the
whole pipeline end-to-end.

## One-shot build

```bash
patches/sts2/build.sh /path/to/SlayTheSpire2.pck [work/SlayTheSpire2.astc.pck]
```

Takes ~40 min on an M-series Mac, ~5 GB scratch space in `work/`. Output is
the repacked pck ready to push to the device.

## What each patch fixes

### `overlay/addons/fmod/fmod.gdextension`

Adds `linux.{editor,debug,release}.arm64` entries so godot picks up the
arm64 fmod GDExtension build instead of bailing with "no library for
linux.arm64". The arm64 `.so` itself lives on the device filesystem at
`/$portdir/addons/fmod/libs/linux/libGodotFmod.linux.template_release.arm64.so`
(provided by the csharp-godot-arm64-kit, not bundled in the pck).

### `overlay/addons/sentry/SentryInit.gd`
### `overlay/addons/sentry/user_feedback/user_feedback_form.gd`

The Sentry GDExtension ships only x86_64 / win / mac. On arm64 it never
loads, so any .gd that references `SentrySDK` / `SentryEvent` / `SentryOptions`
fails to parse. These two were the only callers in the StS2 source — both
get replaced with empty stubs (Node / Control) so the class indexer is
happy and the scene tree still resolves.

### `apply.sh` inline edits

- Drops `SentryInit=...` autoload line from `project.godot` (otherwise
  godot tries to autoload the stub on startup and prints noise).
- Deletes `addons/sentry/sentry.gdextension` so godot stops looking for
  a non-existent arm64 library at startup.

## When to refresh these patches

If a StS2 update changes:
- the fmod GDExtension layout → re-derive `fmod.gdextension` from the
  new pck (extract → grep for the platform entries) and re-add arm64.
- the Sentry integration surface (new .gd files referencing SentrySDK)
  → grep for the new files and add stubs to `overlay/`.
