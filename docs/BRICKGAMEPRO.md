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

BrickGamePro now uses the plugin-driven UGUI Canvas path only. The
`brickgamepro` plugin keeps the original Unity present path intact and does not
enable the generic GPU present viewport. This avoids the real-device black
screen seen with the default-framebuffer GPU copy/crop path.

The old root-Canvas-only experiment was misleading: the asset's root Canvas is
a world-space camera calibration object with serialized local scale around
`0.004297`, while the visible layout is under `AppContent` and `Pad`. Assigning
`(2,2,1)` to the root does not change the composition. The current branch-scale
Canvas path:

1. Hooks `GUIManager.Start()`.
2. Enumerates the root Canvas children with `Transform.GetChild`,
   `Component.get_gameObject`, and `Object.get_name`.
3. Resolves `AppContent` and `Pad` by their serialized names.
4. Captures each branch's initial local scale once, then applies a relative
   2x scale and a calibrated vertical anchored position.

Real-device testing found that this Canvas branch changes the outer UGUI layout
but not the game's dynamic black block content. The `Grid` RectTransform is only
the pale background grid image; changing it does not move the active game
objects. Container probes then confirmed the active blocks are ordinary
world-space Unity objects under the scene camera. For example, forcing Tank mode
with `BRICKGAME_PROBE_START=1` logs `TankPlayer(Clone)` as a root Transform with
grid-like `localPosition=(4,9,0)`, not as a child of the UGUI `Grid`.

The current layout fix therefore remains Canvas-first, but it also adjusts the
world camera in the plugin. `Camera.main` is kept on the normal Unity render
path, its orthographic size is divided by `kCanvasScale`, and its local position
is offset so the world-space blocks line up with the enlarged UGUI board. This
is not GPU present crop: the framebuffer is not copied or cropped at present
time.

Latest real-device feedback: after matching the dynamic block size to the pale
Grid background cells, the dynamic playfield was still offset toward the upper
left. Because this content is world-space and seen through an orthographic
camera, the visible motion is opposite the camera movement. The plugin now uses
`kCanvasScale=2.2`, `kCanvasPositionY=-1190`,
`kWorldCameraOffsetX=0.0`, and `kWorldCameraOffsetY=10.60`. The camera offset
was derived from the previous `(+2.5, -4.0)` value plus a requested visible
correction of about `+2.5` cells right and `-13.75` cells down, followed by
the latest real-device request to move the game area down another `0.25` cell.

### UGUI Canvas方案交接说明

目标：让 Unity/UGUI 自己完成放大和裁切，不再执行 present viewport。目标
效果仍是保持 4:3 比例、放大约 2 倍、显示游戏上半部分，并把下半部分
虚拟按钮推出视口。

当前实验代码在
`projects/unityloader/plugins/brickgamepro/brickgamepro.cpp`，由
`kUseCanvasLayout` 控制，当前值为 `true`。离线解析和运行时验证已经确认：

- `level0` 中 root `Canvas` 的两个实际内容分支是 `AppContent` 和 `Pad`；
- `GUIManager.Start()` hook：以 `this` 调用 `Component.get_transform()`；
- 运行时 `Transform.GetChild()` 枚举得到 `Pad`、`AppContent`，与资源层级一致；
- `RectTransform.get/set_anchoredPosition()`；
- `Transform.set_localScale()`；
- `Screen.get_height()`，用于容器尺寸诊断。

容器中已看到：

```text
hooked GUIManager.Start(0)
Canvas RectTransform layout armed
Canvas child[0] Pad=...
Canvas child[1] AppContent=...
Canvas target AppContent=... scale=(2.000,2.000,2.000)
Canvas target Pad=... scale=(2.000,2.000,2.000)
Grid RectTransform found rt=...
Grid adjusted extraScale=1.0 anchored=(0.0,0.0) scale=(1.000,1.000,1.000)
camera base reason=frame pos=(7.61,-1.00,-10.00) size=25.000
```

Canvas 方案的关键不是修改 root Canvas 的绝对尺寸，而是修改两个可见
内容分支的相对 Transform。`kCanvasPositionY` 是游戏逻辑坐标，不按物理
屏幕像素缩放。`Grid` 节点现在只记录和保持原 scale，因为它是真机已验证的
背景图，不是真正动态棋盘。

动态黑块的修正点是 `Camera.main`：容器中对比确认，单独 Canvas 放大后，
动态块仍偏右上；应用相机正交缩放后，Tank 模式的 world-space 方块进入放大
后的棋盘格。调试入口 `BRICKGAME_PROBE_START=1` 会强制调用
`GameManager.PlayTank()` 以便容器截图验证；默认真机路径不会自动进游戏，也
不会安装这些玩法探针 hook。

注意：Canvas 方案的所有标题常量、hook 和对象探测都应继续留在
`brickgamepro` plugin；只有 IL2CPP 通用解析、GPU viewport、触摸注入等
不含 BrickGamePro 名称和坐标的部分才放在 core。

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
| Y | Fire / A fallback | `(393, 377)` |
| Start | Start / Pause | `(334, 307)` |
| X | Sound | `(367, 307)` |

The original top-right buttons are Settings `(400, 307)` and Exit Game
`(433, 307)`. They are deliberately not mapped to physical controller buttons,
so normal handheld input cannot open Settings or trigger the in-game exit
button.
The original game has no separate Y touch button; Y therefore aliases the
Fire/A touch target.

The touch-coordinate table remains unchanged by the Canvas layout. The plugin
now compensates the nine button RectTransforms for the Canvas scale/translation
and sets their `CanvasRenderer` color alpha to zero. The visible button artwork
and labels are hidden, while the Unity UI hit regions remain at the original
640x480 touch coordinates. Container replay confirms the original coordinates
and multi-pointer `ACTION_POINTER_DOWN/UP` sequence. Real-device testing now
confirms the mapped controls work, except for the Tetris Down feel tracked below.

Guide/Menu is reserved for skin switching. It does not inject a Settings touch:
the `brickgamepro` plugin calls Unity's exported `UnitySendMessage` entry point
against the `Canvas` GameObject, where the `GUIManager` component is attached.
The first Guide press chooses Black because the default skin is already Blue.
The built-in cycle is Blue, Black, Pink, Green, Yellow, Red, Purple, Orange,
Blue2, Green2, Silver. These names and the Guide mapping are plugin constants,
not TOML settings.

Implementation:

1. Keep gameplay touch mapping limited to D-pad, A/Y, Start/Pause, and Sound.
2. Leave Settings and Exit Game unbound from ordinary controller buttons.
3. On Guide/Menu down, schedule one skin-change request for the next render
   frame.
4. Resolve `UnitySendMessage` from `libunity.so` and call the next
   `Canvas.ChangeTo*Skin()` message.
5. Direct managed input is currently disabled after real-device testing showed
   it broke menu/gameplay routing for D-pad/A and made B exit the process.
   D-pad, A/Y, Start, and X therefore use the plugin touch path again. B is
   deliberately unmapped.
6. After every injected touch event and every held-touch refresh, the plugin
   reapplies hidden button graphics. This suppresses Unity UI pressed-state
   visuals such as the Pause or Audio button reappearing while held.
7. For held gameplay buttons, refresh active plugin touches every render frame.
   The generic touch injector converts
   repeated down events for an already-held pointer into `ACTION_MOVE`, so Unity
   sees a continuous touch stream instead of a single down edge followed by
   silence.
8. On physical button release, defer the injected touch `ACTION_UP` until the
   touch has lived for at least 35 ms and at least 1 render frame. This keeps
   quick physical taps from collapsing into a down/up pair that Unity samples
   inside one frame.
9. Keep the loader's normal Start+Select exit hotkey policy unchanged. BrickGamePro
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

Quick taps get one additional title-specific guard: a physical release only
schedules the touch release. The plugin sends the real `ACTION_UP` after the
touch has crossed both the minimum time and frame thresholds. The common
MotionEvent stub also exposes `getDownTime()` and the plugin injector preserves
one down-time across a multi-touch sequence, matching Android's input contract
more closely for click/hold calculations.

## Android Key Events

The generic `controller_key_events=true` path converts SDL controller input
into Android `KeyEvent` objects and passes them to Unity's
`nativeInjectEvent`; it is not native SDL game input. BrickGamePro's inspected
controls are Unity UI touch buttons, so the default remains
`controller_key_events=false` while the plugin touch path is active.

Do not treat Android key injection as a gameplay fix without confirming that
this title handles those key events.

## Feedback Popup

The original game can randomly show a `FeedBackMenu` / "do you like this game?"
rating prompt. The plugin blocks this at two levels: it hooks
`GUIManager.ShowFeedBackMenu()` and `GUIManager.LeaveFeedBack()` as no-ops, and
it keeps the runtime `FeedBackMenu` and `LeaveFeedBackBtn` GameObjects inactive.
This is BrickGamePro-specific and stays in the plugin.

## Remaining Issues

The current real-device status is:

- D-pad, A/Y, Start/Pause, X/Sound, and Guide/Menu skin switching are routed
  through the BrickGamePro plugin.
- B is intentionally unmapped because testing showed the previous B pause path
  could exit the process on device.
- The Canvas/camera layout is visually correct on the tested 4:3 TrimUI panel
  after `kCanvasPositionY=-1190` and `kWorldCameraOffsetY=10.60`.
- The Tetris Down feel is still not equivalent to the Android APK. Short Down
  taps mostly work but can still occasionally be missed, and held Down still
  drops too aggressively, making it hard to stop fast descent in mid-air. This
  is deferred for a later pass; do not re-enable the current direct managed
  input experiment as a quick fix because it caused real-device input routing
  regressions.

## Build And Test Notes

Device builds should be made in the `GlES_Dev` container and must keep:

```text
-DJNIVM_ENABLE_RETURN_NON_ZERO=OFF
```

Configure the cached device build explicitly before compiling:

```sh
cmake -S . -B build-device-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DBD_ENABLE_LOG=ON -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF \
  -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF
```

Build both the loader and the BrickGamePro plugin:

```text
cmake --build build-device-release --target unityloader brickgamepro.so
```

Deploy `unityloader`, `unity.toml`, and `unityloader.d/brickgamepro.so`.
No FFmpeg runtime libraries or `LD_LIBRARY_PATH` override are required.

For the current Windows-hosted container workflow, copy the edited plugin into
the `GlES_Dev` container, build only the plugin when the loader/core did not
change, run the container replay, then pull the plugin artifact back to the
local staging directory:

```powershell
docker cp projects\unityloader\plugins\brickgamepro\brickgamepro.cpp GlES_Dev:/workspace/Bogodroid/projects/unityloader/plugins/brickgamepro/brickgamepro.cpp
docker exec GlES_Dev bash -lc "cd /workspace/Bogodroid && cmake --build build-device-release --target brickgamepro.so -j2"
docker exec GlES_Dev bash -lc "cp /workspace/Bogodroid/build-device-release/unityloader.d/brickgamepro.so /game/BrickGamePro/unityloader.d/brickgamepro.so && rm -rf /game/BrickGamePro/seq-startpause-1190 && BRICKGAME_PROBE_START=1 GAME_DIR=/game/BrickGamePro W=640 H=480 SECS=45 DUMP_AT=500 CAP_MS=1000 SEQ_DIR=/game/BrickGamePro/seq-startpause-1190 bash /workspace/Bogodroid/scripts/container-run-game-seq.sh"
docker cp GlES_Dev:/workspace/Bogodroid/build-device-release/unityloader.d/brickgamepro.so deploy\BrickGamePro\unityloader.d\brickgamepro.so
```

Useful log check after replay:

```powershell
docker exec GlES_Dev bash -lc "grep -E 'plugin armed|Canvas target|camera base|hooked GUIManager\.(ShowFeedBackMenu|LeaveFeedBack)|feedback menu|leave feedback|OpenGL Renderer|probe auto-start|start -> touch|b -> touch' -n /game/BrickGamePro/log-seq.txt | tail -n 140"
```

Expected log signs: `directInput=0`, `canvasScale=2.2 canvasY=-1190`,
`start -> touch 334.0,307.0`, no `b -> touch 334.0,307.0`, and feedback popup
hooks still installed.

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
