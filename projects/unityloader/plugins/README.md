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

Plugins must verify `abi_version` and `struct_size` before accessing the API.
Build the loader and deployed plugins from the same Plugin ABI revision.

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

- `hollow_knight_viewport`: title-specific viewport and camera fixes.
- `terraria_autoname`: title-specific character naming support.
- `samurai2_offline`: offline Madfinger Google Play JNI implementation. It
  requires `[google_play] offline = true` in the game's TOML file.

JNI callbacks must not allow C++ exceptions to cross the C ABI boundary.
Passing null as the module to `so_symbol` searches all loaded Android modules;
passing `api->il2cpp` restricts the lookup to that module.

## Present callbacks (ABI v3)

Plugins that need work on the Unity render/present thread (for example polling
an `AsyncOperation` while `BD_EGL_CPU_PRESENT` is armed) must register via:

```c
api->register_present_callback(my_cb, userdata);
```

Do **not** export game-specific symbols for the core to `dlsym`. The host calls
`bd_plugin_run_present_callbacks()` from `eglSwapBuffers`.

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
