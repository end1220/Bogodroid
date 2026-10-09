# BrickGamePro Port Notes

## Branch Status

- Branch: `BrickGame`.
- Base: this branch has been advanced to `origin/oddmar` through `a091250`.
- BrickGamePro-specific behavior is isolated in `projects/unityloader/plugins/brickgamepro/`.
- Shared work stays in `unityloader`: multi-pointer `MotionEvent`, plugin touch injection, plugin-set present viewport, and display-size probing.
- Staging directory: `deploy/BrickGamePro/` is local-only and ignored by git.

## Runtime Layout

- Port directory on TrimUI Smart Pro: `/mnt/SDCARD/Data/ports/BrickGamePro`.
- Ports launcher: `/mnt/SDCARD/Roms/PORTS/BrickGamePro.sh`.
- Runtime config: `configs/brickgamepro.toml`, deployed as `unity.toml`.
- Game package: `com.perseusgames.brickgamepro`, Unity 2018.2 IL2CPP.
- Required plugin: `unityloader.d/brickgamepro.so`.

Do not deploy Oddmar plugins for BrickGamePro. Do not add Oddmar/FFmpeg
`LD_LIBRARY_PATH` entries to `BrickGamePro.sh`; this port does not need Android
video playback or FFmpeg runtime libraries.

## Display And Viewport

The config intentionally leaves `displayWidth=0` and `displayHeight=0`.
Bogodroid resolves the actual display size from SDL/fb0 at runtime, so the same
TOML can run on 640x480, 1024x768, or another 4:3 handheld panel without
hard-coding the resolution.

BrickGamePro is presented through the generic EGL/SDL present-crop path. The
`brickgamepro` plugin enables and configures it:

```toml
[game_patches.brickgamepro.viewport]
enabled=true
renderScale=2.0
anchor="top"
offsetY=-96
```

The game framebuffer is kept at its original aspect ratio and uniformly scaled.
The viewport shows the upper part of the game and pushes the lower virtual-button
area outside the visible panel; `offsetY=-96` moves the result upward by about
one fifth of a 480 px reference screen.

## Controller Model

BrickGamePro's gameplay controls are Unity UI touch buttons, so this port uses
the `brickgamepro` plugin to map physical controller buttons to Android
`MotionEvent` touch input:

```toml
[input]
controller=true
touch_mode=true
dpad_synthesize_hat=false
controller_key_events=false

[game_patches.brickgamepro.touch]
enabled=true
design_width=640.0
design_height=480.0
```

The plugin coordinates are measured in the original 640x480 BrickGamePro layout.
At injection time Bogodroid scales them to the current detected display size, so
a 1024x768 panel uses a 1.6x scale and maps `a=(393,377)` to roughly
`(628.8,603.2)`.

Mapped original touch buttons:

| Controller input | Original touch button | 640x480 point |
| --- | --- | --- |
| D-pad Left | Left | `(222, 375)` |
| D-pad Right | Right | `(300, 375)` |
| D-pad Up | Up | `(260, 335)` |
| D-pad Down | Down | `(260, 414)` |
| A | Fire / A | `(393, 377)` |
| B | B | `(334, 307)` |
| X | X | `(367, 307)` |
| Y | Y | `(400, 307)` |
| Start | Start | `(433, 307)` |

## Multi-Touch Fix

Single-pointer touch emulation caused a sticky-fire failure: holding A, pressing
a direction, then releasing one of them could leave the game thinking A was still
pressed. The generic input backend now exposes plugin touch injection, tracks
active plugin pointers, and emits Android-style multi-pointer events:

- first press: `ACTION_DOWN`
- additional press: `ACTION_POINTER_DOWN`
- release while another touch remains: `ACTION_POINTER_UP`
- final release: `ACTION_UP`

The MotionEvent stub reports multiple pointers and implements `getPointerId()`,
`getX(index)`, and `getY(index)`. This keeps A and D-pad presses independent for
Unity's touch handling.

## SDL Key Events

Direct SDL/controller key events are still available behind
`controller_key_events=true`, but BrickGamePro listens to its on-screen Unity UI
buttons for gameplay. The default remains `controller_key_events=false` to avoid
double input while the plugin touch path is active.

Use the SDL key path only as an A/B diagnostic if plugin multi-touch still fails
on device.

## Build And Test Notes

Device builds should be made in the `GlES_Dev` container and must keep:

```text
-DJNIVM_ENABLE_RETURN_NON_ZERO=OFF
```

Build both the loader and the BrickGamePro plugin:

```text
cmake --build build-device-release --target unityloader plugin_brickgamepro
```

Deploy `unityloader`, `unity.toml`, and `unityloader.d/brickgamepro.so`.
No FFmpeg runtime libraries or `LD_LIBRARY_PATH` override are required.

Useful container replay for the sticky-fire scenario:

```sh
BD_PAD_REPLAY="2500:a,2700:dpright" \
BD_PAD_REPLAY_HOLD=1000 \
timeout -s INT 12 ./unityloader unity.toml
```

Expected touch sequence:

```text
A DOWN          action=0   pointers=1
DP_RIGHT DOWN   action=261 pointers=2
A UP            action=6   pointers=2
DP_RIGHT UP     action=1   pointers=1
```

On device, retest from the Ports menu:

- hold and release A alone;
- hold A plus a direction, release A first;
- hold A plus a direction, release the direction first;
- confirm firing stops after A is released.
