# BrickGamePro Port Notes

## Branch Status

- Branch: `BrickGame`.
- Base: this branch has been advanced to `origin/oddmar` through `a091250` (`Make FFmpeg a runtime media dependency`).
- The BrickGamePro-specific work now lives on top of the Oddmar line, not on the old ARM32/Mono path.
- Staging directory: `deploy/BrickGamePro/` is local-only and ignored by git; commit source/config/docs instead.

## Runtime Layout

- Port directory on TrimUI Smart Pro: `/mnt/SDCARD/Data/ports/BrickGamePro`.
- Ports launcher: `/mnt/SDCARD/Roms/PORTS/BrickGamePro.sh`.
- Runtime config: `configs/brickgamepro.toml`, deployed as `unity.toml`.
- Game package: `com.perseusgames.brickgamepro`, Unity 2018.2 IL2CPP.

## Display And Viewport

The config intentionally leaves `displayWidth=0` and `displayHeight=0`.
Bogodroid resolves the actual display size from SDL/fb0 at runtime, so the same TOML can run on 640x480, 1024x768, or another 4:3 handheld panel without hard-coding the resolution.

BrickGamePro is presented through the EGL/SDL present-crop path:

```toml
[viewport]
enabled=true
renderScale=2.0
anchor="top"
offsetY=-96
```

The game framebuffer is kept at its original aspect ratio and uniformly scaled. The viewport shows the upper part of the game and pushes the lower virtual-button area outside the visible panel; `offsetY=-96` moves the result upward by about one fifth of a 480 px reference screen.

## Controller Model

BrickGamePro's gameplay controls are Unity UI touch buttons, so this port defaults to physical controller buttons driving Android `MotionEvent` touch input:

```toml
[input]
controller=true
touch_mode=true
dpad_synthesize_hat=false
controller_key_events=false

[input.controller_touch]
dpleft=[222, 375]
dpright=[300, 375]
dpup=[260, 335]
dpdown=[260, 414]
a=[393, 377]
b=[334, 307]
x=[367, 307]
y=[400, 307]
start=[433, 307]
```

These coordinates are measured in the original 640x480 BrickGamePro layout. At injection time Bogodroid scales them to the current detected display size, so a 1024x768 panel uses a 1.6x scale and maps `a=[393,377]` to roughly `(628.8,603.2)`.

Mapped original touch buttons:

| Controller input | Original touch button |
| --- | --- |
| D-pad Left | Left |
| D-pad Right | Right |
| D-pad Up | Up |
| D-pad Down | Down |
| A | Fire / A |
| B | B |
| X | X |
| Y | Y |
| Start | Start |

## Multi-Touch Fix

Single-pointer touch emulation caused a sticky-fire failure: holding A, pressing a direction, then releasing one of them could leave the game thinking A was still pressed. The input backend now tracks active controller touches and emits Android-style multi-pointer events:

- first press: `ACTION_DOWN`
- additional press: `ACTION_POINTER_DOWN`
- release while another touch remains: `ACTION_POINTER_UP`
- final release: `ACTION_UP`

The MotionEvent stub now reports multiple pointers and implements `getPointerId()`, `getX(index)`, and `getY(index)`. This keeps A and D-pad presses independent for Unity's touch handling.

## SDL Key Events

Direct SDL/controller key events are still available behind `controller_key_events=true`, but BrickGamePro appears to listen to its on-screen Unity UI buttons for gameplay. The default remains `controller_key_events=false` to avoid double input while the touch path is active.

Use the SDL key path only as an A/B diagnostic if multi-touch still fails on device.

## Build And Test Notes

Device builds should be made in the `GlES_Dev` container and must keep:

```text
-DJNIVM_ENABLE_RETURN_NON_ZERO=OFF
```

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
