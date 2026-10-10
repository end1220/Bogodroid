# BrickGamePro Port Notes

## Branch Status

- Branch: `BrickGame`.
- Base: this branch has been advanced to `origin/oddmar` through `a091250`.
- BrickGamePro-specific behavior is isolated in `projects/unityloader/plugins/brickgamepro/`.
- Shared work stays in `unityloader`: multi-pointer `MotionEvent`, plugin touch injection, optional generic present viewport support, and display-size probing.
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

BrickGamePro uses a plugin-driven GPU present viewport. The `brickgamepro`
plugin enables the generic viewport pass with title-owned constants:
`renderScale=2.0`, `anchor=top`, `offsetY=-60`, and nearest-neighbor sampling.
The game image is kept at its original aspect ratio, uniformly enlarged, and
cropped so the lower virtual-button area falls outside the 4:3 handheld panel.

This replaces the old CPU readback implementation:

- no per-frame `glReadPixels`;
- no CPU framebuffer staging or texture re-upload;
- the source framebuffer is copied to a GL texture with `glCopyTexSubImage2D`;
- the crop shader samples that GPU texture directly.

The attempted native UGUI Canvas/RectTransform path is kept disabled in the
plugin for now. In container testing, hooks for `GUIManager.Start()` and
`CanvasScaler.HandleScaleWithScreenSize()` resolved, but changing the root
Canvas transform and CanvasScaler scale factor did not change the visible
composition. The shipped path therefore uses the reliable pure-GPU viewport
copy/crop.

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
```

The plugin owns its fixed BrickGamePro layout constants. Coordinates are measured
in the original 640x480 game layout and are scaled by the generic touch injector
to the current detected display size.
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
| B | Start / Pause | `(334, 307)` |
| X | Sound | `(367, 307)` |

The original top-right buttons are Settings `(400, 307)` and Exit Game
`(433, 307)`. They are deliberately not mapped to physical controller buttons,
so normal handheld input cannot open Settings or trigger the in-game exit
button.

Guide/Menu is reserved for skin switching. It does not inject a Settings touch:
the `brickgamepro` plugin calls Unity's exported `UnitySendMessage` entry point
against the `Canvas` GameObject, where the `GUIManager` component is attached.
The first Guide press chooses Black because the default skin is already Blue.
The built-in cycle is Blue, Black, Pink, Green, Yellow, Red, Purple, Orange,
Blue2, Green2, Silver. These names and the Guide mapping are plugin constants,
not TOML settings.

Implementation:

1. Keep gameplay touch mapping limited to D-pad, A, Start/Pause, and Sound.
2. Leave Settings and Exit Game unbound from ordinary controller buttons.
3. On Guide/Menu down, schedule one skin-change request for the next render
   frame.
4. Resolve `UnitySendMessage` from `libunity.so` and call the next
   `Canvas.ChangeTo*Skin()` message.
5. For held gameplay buttons, refresh active plugin touches every render frame.
   The generic touch injector converts repeated down events for an already-held
   pointer into `ACTION_MOVE`, so Unity sees a continuous touch stream instead
   of a single down edge followed by silence.
6. Keep the loader's normal Start+Select exit hotkey policy unchanged. BrickGamePro
   does not disable it in TOML; accidental exits should be handled separately
   only if real-device testing proves they are a problem.

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

Held touches are also refreshed on each render frame. When `hold_refresh=true`,
the BrickGamePro plugin re-injects every active pointer; unityloader recognizes
that the pointer is already down and emits `ACTION_MOVE` rather than another
`ACTION_DOWN`/`ACTION_POINTER_DOWN`. This targets the real-device symptom where
holding a direction or Fire felt delayed and less continuous than the Android
APK's native touch behavior.

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
...
A MOVE          action=2   pointers=1
DP_RIGHT DOWN   action=261 pointers=2
...
A MOVE          action=2   pointers=2
A UP            action=6   pointers=2
DP_RIGHT UP     action=1   pointers=1
```

On device, retest from the Ports menu:

- hold and release A alone;
- hold A plus a direction, release A first;
- hold A plus a direction, release the direction first;
- confirm firing stops after A is released.
