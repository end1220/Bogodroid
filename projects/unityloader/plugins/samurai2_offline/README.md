# Samurai2 offline plugin

This plugin supplies the game-specific, offline implementation of Madfinger's
`com/madfingergames/googleplaygames/*` JNI API. It registers those classes
during the loader's early JNI plugin phase and reports Play Games as
unavailable without requiring Google services or a network connection.

Enable it with `[google_play] offline = true` and place `samurai2_offline.so`
in the game's `unityloader.d` directory. Generic JNI, LVL interfaces,
controller remapping, and Unity loading remain in the core loader.

```toml
[input]
controller_name = "Microsoft X-Box 360 pad"

[input.remap]
guide = "ESCAPE"

[google_play]
offline = true
```

```text
Samurai2/
  unityloader
  samurai2.toml
  gamedata/
  unityloader.d/
    samurai2_offline.so
```

The plugin reports Play Games as unavailable, returns empty player data, and
turns achievement, leaderboard, recording, and UI requests into offline
no-ops. It does not require Google services or network access.

The ARM64 IL2CPP build updated on October 2, 2025 was verified on an Anbernic
Linux handheld. Keep the plugin and loader on the same Plugin ABI revision.
