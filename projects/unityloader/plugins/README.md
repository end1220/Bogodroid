# Unityloader plugins

Unityloader loads game-specific shared libraries from `unityloader.d/*.so`,
relative to the directory containing the active TOML configuration file.
Plugins keep title-specific compatibility code out of the core loader.

## Lifecycle

Each plugin exports `bogodroid_plugin_init(const BogoPluginApi*)`. The host may
call it in two phases:

1. Early JNI phase: `api->jvm` is available and `api->il2cpp` is null.
2. IL2CPP phase: both `api->jvm` and `api->il2cpp` are available.

A plugin that requires IL2CPP must return `BOGO_PLUGIN_DEFERRED` during the
early phase. The host closes that temporary handle and retries after IL2CPP is
loaded. Return `BOGO_PLUGIN_OK` only after initialization is complete; any
other result is treated as an error.

After initialization, plugins may subscribe to later module loads. A module
callback registered through `api->register_module_loaded(name, cb, userdata)`
is invoked immediately if the requested module is already present, otherwise
when the host registers it after `so_load`.

Plugins must verify `abi_version` and `struct_size` before accessing the API.
Build the loader and deployed plugins from the same Plugin ABI revision. The
current ABI is v8.

## Build and deploy

Build every bundled plugin or one specific plugin:

```bash
cmake --build build --target unityloader_plugins
cmake --build build --target plugin_samurai2_offline
```

Deploy only the plugins needed by a game:

```text
MyGame/
  unityloader
  game.toml
  unityloader.d/
    samurai2_offline.so
```

Bundled plugins:

- `brickgamepro`: BrickGamePro UGUI Canvas layout hooks, Guide skin switching,
  and physical-controller to on-screen-touch mapping. Its title constants live
  in the plugin, not TOML.
- `hollow_knight_viewport`: title-specific viewport and camera fixes.
- `oddmar_input`: Oddmar-specific input gate and IL2CPP input hooks.
- `oddmar_video`: Oddmar `libunity.so+0x4c124c` asset-path translation. It is
  enabled only when `BD_BYPASS_VIDEO_TRANSLATE` is set and resolves paths from
  the loader's current game-data directory.
- `terraria_autoname`: title-specific character naming support.
- `samurai2_offline`: offline Madfinger Google Play JNI implementation. It
  requires `[google_play] offline = true` in the game's TOML file.
- `skul_pad`: **archived** Skul PAD/bootstrap hooks (port stopped; keep
  `game_patches.skul_pad.enabled = false` unless replaying that case study).

JNI callbacks must not allow C++ exceptions to cross the C ABI boundary.
Passing null as the module to `so_symbol` searches all loaded Android modules;
passing `api->il2cpp` restricts the lookup to that module.

## Module lookup (ABI v6)

Plugins that need a module other than IL2CPP can query or subscribe by library
name:

```c
BogoSoModule* unity = api->find_module("libunity.so");
api->register_module_loaded("libunity.so", my_cb, userdata);
```

The name matches the module soname, exact path, or basename. Registered module
callbacks may run immediately during plugin init if the module is already
loaded, so callbacks must be ready for either timing.

## Present callbacks (ABI v6)

Plugins that need work on the Unity render/present thread (for example polling
an `AsyncOperation` while `BD_EGL_CPU_PRESENT` is armed) must register via:

```c
api->register_present_callback(my_cb, userdata);
```

Do **not** export game-specific symbols for the core to `dlsym`. The host calls
`bd_plugin_run_present_callbacks()` from `eglSwapBuffers`.

Oddmar's `AssetLocator`/`AssetReader` is provided by `oddmar_assetlocator.so`.
It uses the v7+ host-owned C ABI for object userdata, byte arrays, string arrays,
and object construction. The plugin must be deployed beside `unityloader` in
`unityloader.d/`; the core `BD_ENABLE_MOBGE_ASSETLOCATOR` implementation remains
available as an A/B and rollback path. Plugins must not include jnivm C++ headers
or share C++ descriptors across the DSO boundary.

The A3 container acceptance run on 2026-10-08 used
`BD_ENABLE_MOBGE_ASSETLOCATOR=OFF` and `JNIVM_ENABLE_RETURN_NON_ZERO=OFF`.
It ran the plugin path for about 30 seconds, reached `bundle1` through
`bundle8`, produced loader GL frame dumps, and had no `BD-SEGV` or archive
header errors. No TrimUI deployment was performed because the device was
offline.

## Input observers (ABI v6)

Plugins that need raw SDL controller events can register a `BogoInputObserver`
through `api->register_input_observer`. The observer receives controller button
edges, axis values, and render-frame boundaries before the Android compatibility
path filters or remaps the event. Callbacks must stay bounded and must not throw
across the C ABI boundary. Plugins that install RVA hooks should use
`api->so_base(api->il2cpp)` for the opaque module's load base.

## Present viewport (ABI v8)

Plugins can request a generic present-crop pass through
`api->set_present_viewport(enabled, renderScale, anchor, offsetY)`.
The host copies the rendered GL frame into a texture with
`glCopyTexSubImage2D`, applies the crop shader, and swaps the SDL window
without CPU pixel readback.

Newer v8 headers also expose the backward-compatible optional helper
`api->set_present_viewport_filter(enabled, renderScale, anchor, offsetY, filter)`.
`filter` accepts `linear` or `nearest`; use `nearest` for pixel-art or
grid-heavy ports where linear sampling can erase one-pixel lines during scaling.

## Plugin logging

Plugins use the host `api->log` callback and follow the loader's compile-time
`BD_ENABLE_LOG` switch. There is no runtime `[logging]` category filter.

`hollow_knight_viewport` keeps hook/install/failure messages enabled, but gates
its bounded camera and tk2d telemetry behind the plugin's existing `debug`
setting:

```toml
[game_patches.hollow_knight_viewport]
debug = true
```

Leave this absent or `false` for normal ports. Enable it only for a viewport
diagnostic run, then disable it again so repeated camera callbacks do not expand
the game log.
