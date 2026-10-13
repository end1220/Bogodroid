# BrickGamePro Port Notes

## Branch Status

- Branch: `BrickGame`.
- Base: this branch has been advanced to `origin/oddmar` through `a091250`.
- Current workspace: `D:\Locke\gitee\Bogodroid-brickgame`.
- The BrickGame linked worktree was moved here on October 12, 2026. Use this
  directory for builds, container staging, commits, and pushes.
- BrickGamePro-specific behavior is isolated in `projects/unityloader/plugins/brickgamepro/`.
- Shared work stays in `unityloader`: multi-pointer `MotionEvent`, plugin touch injection, optional generic present viewport support, display-size probing, and the `plugin_il2cpp.h` metadata/hook helpers.
- Staging directory: `deploy/BrickGamePro/` is local-only and ignored by git.

## Runtime Layout

- Current Anbernic deployment (2026-10-12, `172.16.5.40`):
  `/mnt/mmc/Roms/PORTS/BrickGamePro`, launcher
  `/mnt/mmc/Roms/PORTS/BrickGamePro.sh`.
- Previous TrimUI Smart Pro layout: `/mnt/SDCARD/Data/ports/BrickGamePro`,
  launcher `/mnt/SDCARD/Roms/PORTS/BrickGamePro.sh`.
- Runtime config: `configs/brickgamepro.toml`, deployed as `unity.toml`.
- Anbernic launcher source: `scripts/brickgamepro_launch.sh`, based on the
  device's existing Maximus2/Samurai2 scripts. It creates `conf/shared_prefs`
  and `cache`, loads the controller database, selects Pulse/ALSA, and runs the
  loader in the foreground with output in `BrickGamePro/log.txt`.
- Game package: `com.perseusgames.brickgamepro`, Unity 2018.2 IL2CPP.
- Required plugin: `unityloader.d/brickgamepro.so`.

Do not deploy Oddmar plugins for BrickGamePro. Do not add Oddmar/FFmpeg
`LD_LIBRARY_PATH` entries to `BrickGamePro.sh`; this port does not need Android
video playback or FFmpeg runtime libraries.

The complete Anbernic install has 144 runtime files verified against
`SHA256SUMS`; launcher, loader, config, and Down-fix plugin hashes also match
local staging. The user confirmed that the original Down fix improved device
feel. The subsequent 100 ms / 60 ms timing and font correction passed container
checks and were deployed together. The current plugin uses 100 ms / 30 ms
with the font correction retained, per-piece Down hold ownership, A dismissal
of Game Over, persistent hiding of touch-button graphics, A/B Yes/No handling
in ExitMenu, and Select skin switching disabled. This build was
deployed on 2026-10-13; all 144 runtime files passed SHA-256 verification.
Its validation and deployment status are recorded in
[UI analysis](BRICKGAMEPRO_UI_ANALYSIS.md). Prior plugins are backed up outside
unityloader.d. No remote game launch was performed during deployment.
`init_config` changes cwd to `game/`, so Android file/cache paths are
`../conf/` and `../cache/`; source/package/OBB paths are `./`. The corrected
deployment config passed a container replay and PlayerPrefs save.

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
a camera calibration object with serialized local scale around
`0.004297`, while the visible layout is under `AppContent` and `Pad`. Assigning
`(2,2,1)` to the root does not change the composition. The current branch-scale
Canvas path:

1. Hooks `GUIManager.Start()`.
2. Enumerates the root Canvas children with `Transform.GetChild`,
   `Component.get_gameObject`, and `Object.get_name`.
3. Resolves `AppContent` and `Pad` by their serialized names.
4. Captures each branch's initial local scale once, then applies a relative
   2x scale and a calibrated vertical anchored position.

Runtime font inspection confirms the root Canvas uses Screen Space Camera
(renderMode=1). At 640x480, its scaleFactor is 0.1875; AppContent's additional
2.2x transform was magnifying low-resolution dynamic glyphs. The plugin now
adjusts Text.pixelsPerUnit for the HUD and finish scores by the same 2.2 factor,
so Unity generates glyphs at their displayed size while preserving layout.
Container A/B screenshots show clearer text. See
[font analysis and validation](BRICKGAMEPRO_FONT_ANALYSIS.md).

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

当前布局代码在
`projects/unityloader/plugins/brickgamepro/brickgamepro.cpp`，固定使用 Canvas
分支缩放与世界相机调整。离线解析和运行时验证已经确认：

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
camera base reason=frame pos=(7.61,-1.00,-10.00) size=25.000
```

Canvas 方案的关键不是修改 root Canvas 的绝对尺寸，而是修改两个可见
内容分支的相对 Transform。`kCanvasPositionY` 是游戏逻辑坐标，不按物理
屏幕像素缩放。`Grid` 节点现在只记录和保持原 scale，因为它是真机已验证的
背景图，不是真正动态棋盘。

动态黑块的修正点是 `Camera.main`：容器中对比确认，单独 Canvas 放大后，
动态块仍偏右上；应用相机正交缩放后，Tank 模式的 world-space 方块进入放大
后的棋盘格。调试入口 `BRICKGAME_PROBE_START=1` 默认经原 GUI 流程进入 Tank，
`BRICKGAME_PROBE_GAME=Tetris|TetrisFive` 可选择测试玩法；GUI 不可用时才回退
到直接调用 `GameManager.Play*()`。暂停验证必须确认日志 `viaGUI=1`。
默认真机路径不会自动进游戏，也不会安装这些玩法探针 hook。

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

Tetris Down is now a title-specific exception. After resolving and checking
the original IL2CPP field layout, the plugin queues physical Down edges for
the active mino's `CheckUserInput`: one original `MoveDown` on the next Update,
then a 100 ms initial delay and 30 ms repeat interval. Spawning the next mino
cancels repeat while retaining physical-key ownership: the next piece falls
naturally until Down is released and pressed again. Release cancels repeat;
a tap released before Update retains its first step. Menus and non-Tetris
games keep the touch mapping below. `BRICKGAME_DOWN_FIX=0` restores the original
Down touch route for comparison. See [Down analysis and container validation](BRICKGAMEPRO_DOWN_ANALYSIS.md).

These timing constants apply only to the TetrisMino variants (game IDs
1, 10, 11, 12, 14, 19) and TetrisFiveMino (13). Other BrickGamePro games
and menus retain the original Down touch route; other ports do not load
this title-specific plugin.

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
| Guide/Menu | Exit Game | `(433, 307)` |

The original top-right buttons are Settings `(400, 307)` and Exit Game
`(433, 307)`. Settings remains unbound. Guide/Menu simulates a click on the
Exit Game button through the same touch path as the original UI.
The original game has no separate Y touch button; Y therefore aliases the
Fire/A touch target.

The touch-coordinate table remains unchanged by the Canvas layout. The plugin
now compensates the nine button RectTransforms for the Canvas scale/translation
and sets their `CanvasRenderer` color alpha to zero. The visible button artwork
and labels are hidden, while the Unity UI hit regions remain at the original
640x480 touch coordinates. Container replay confirms the original coordinates
and multi-pointer `ACTION_POINTER_DOWN/UP` sequence. Real-device testing now
confirms the mapped controls work; the new Tetris Down fix still needs device validation.

Select skin switching is disabled. A single Select press neither changes the
skin nor injects a Settings touch; Start+Select retains the loader exit hotkey.
The existing skin preference and original skin resources are preserved.

While ExitMenu is visible, A invokes the original Yes button's
`GUIManager.ExitGame()` (Unity `Application.Quit()`), and B invokes the original
No button's `BackSelectGameMenu()`. SDL only queues the request; GUIManager.Update
executes it on the Unity thread. The plugin consumes the matching release even
after B closes the popup. Game Over retains its separate A-to-menu behavior.

Implementation:

1. Keep gameplay touch mapping limited to D-pad, A/Y, Start/Pause, and Sound.
2. Leave Settings unbound; map Guide/Menu to the original Exit Game touch point.
3. Remove the Select skin-change route; Select alone has no plugin action.
4. Bind ExitMenu and resolve its original Yes/No methods for A/B.
5. The old direct managed input experiment was removed after real-device testing showed
   it broke menu/gameplay routing for D-pad/A and made B exit the process.
   D-pad other than the Tetris Down exception, A/Y, Start, X, and Guide use the
   plugin touch path.
   B is unmapped outside ExitMenu.
6. Hook CanvasRenderer.SetColor_Injected and force alpha to zero for discovered
   control graphics and their descendants. This blocks pressed-state tweens
   before they can show Pause, Audio, or other original touch buttons. Layout
   and raycasts remain active; Unity UI changes run on the game thread.
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

## Game Over

A press while FinishMenu is visible schedules GUIManager.BackSelectGameMenu
on the Unity thread. This runs the original Menu-button cleanup and returns
to SelectingGame; the corresponding A release is consumed. FinishMenu's
Share, Best, Menu, Replay, Gift, and rating buttons remain inactive, while its
title and scores remain visible. See [UI analysis and replay evidence](BRICKGAMEPRO_UI_ANALYSIS.md).

## Remaining Issues

The current real-device status is:

- The pre-game Perseus Games splash is an Android Java View displaying
  `res/drawable/unity_static_splash.png`, not a video. The resource is deployed,
  but the loader does not implement that View; engine startup time remains
  visible as blank output. See [splash analysis](BRICKGAMEPRO_SPLASH_ANALYSIS.md).
- D-pad, A/Y, Start/Pause, X/Sound, and Guide/Menu Exit Game are routed through
  the BrickGamePro plugin. Select skin switching is disabled.
- In ExitMenu, A confirms quitting and B cancels; elsewhere B is unmapped.
  The previous B pause experiment has been removed.
- The Canvas/camera layout is visually correct on the tested 4:3 TrimUI panel
  after `kCanvasPositionY=-1190` and `kWorldCameraOffsetY=10.60`.
- The Tetris Down fix is built and verified in the ARM64 container. IL2CPP
  disassembly identified shared vertical timers and a held-Down branch that
  bypasses the repeat timer. The plugin now preserves short taps and schedules
  original single-step moves at 100 ms / 30 ms. Container checks cover release,
  pause/resume, movement/rotation combinations, held-key spawn gating, game over, TetrisFive,
  and Tank fallback. Device feedback confirmed the original 200 ms / 90 ms fix
  improved Down feel; the user subsequently requested 100 ms / 30 ms to match
  the original Tetris repeat interval.
  The new timing still needs device feedback; single-brick,
  bomb, and line-clear scenarios still need dedicated testing. See
  [the evidence and reproduction commands](BRICKGAMEPRO_DOWN_ANALYSIS.md).
  The old direct managed input experiment has been removed.

## Code Ownership And Cleanup (2026-10-13)

`projects/unityloader/plugin_il2cpp.h` provides reusable IL2CPP export binding,
image/type/method/field lookup, method detours, and bounded metadata diagnostics.
It uses the existing v8 C plugin API without sharing C++ state across DSOs.
Game class names, field-layout checks, timing, coordinates, UI policy, and hooks
remain in `plugins/brickgamepro/brickgamepro.cpp`.

Removed retired Select/UnitySendMessage skin routing, direct-input pulse queues,
world-object scaling, Grid extra scaling, unused layout switches, and uninstalled
gameplay probe hooks. The active Canvas/camera path and replay probes remain.
Cleanup validation is recorded in [UI analysis](BRICKGAMEPRO_UI_ANALYSIS.md).

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
  -DIL2CPP_TRACE=OFF \
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
docker cp projects\unityloader\plugin_il2cpp.h GlES_Dev:/workspace/Bogodroid/projects/unityloader/plugin_il2cpp.h
docker exec GlES_Dev bash -lc "cd /workspace/Bogodroid && cmake --build build-device-release --target brickgamepro.so -j2"
docker exec GlES_Dev bash -lc "cp /workspace/Bogodroid/build-device-release/unityloader.d/brickgamepro.so /game/BrickGamePro/unityloader.d/brickgamepro.so && rm -rf /game/BrickGamePro/seq-startpause-1190 && BRICKGAME_PROBE_START=1 GAME_DIR=/game/BrickGamePro W=640 H=480 SECS=45 DUMP_AT=500 CAP_MS=1000 SEQ_DIR=/game/BrickGamePro/seq-startpause-1190 bash /workspace/Bogodroid/scripts/container-run-game-seq.sh"
docker cp GlES_Dev:/workspace/Bogodroid/build-device-release/unityloader.d/brickgamepro.so deploy\BrickGamePro\unityloader.d\brickgamepro.so
```

Useful log check after replay:

```powershell
docker exec GlES_Dev bash -lc "grep -E 'plugin armed|Canvas target|camera base|hooked GUIManager\.(ShowFeedBackMenu|LeaveFeedBack)|feedback menu|leave feedback|OpenGL Renderer|probe auto-start|start -> touch|b -> touch' -n /game/BrickGamePro/log-seq.txt | tail -n 140"
```

Expected log signs: `touch=1 refresh=1`, `canvasScale=2.2 canvasY=-1190`,
`start -> touch 334.0,307.0`, and
`guide -> touch 433.0,307.0`; there must be no `b -> touch 334.0,307.0`.
Feedback popup hooks should still be installed.

Useful container replay for the sticky-fire scenario:

```sh
BD_PAD_REPLAY="2500:a,2700:dpright" \
BD_PAD_REPLAY_HOLD=1000 \
timeout -s INT 12 ./unityloader unity.toml
```

For the Select/Guide mapping replay, use SDL's `back` name for the physical
Select key:

```sh
BD_PAD_REPLAY="2500:back,5000:guide" \
BD_PAD_REPLAY_HOLD=140 \
timeout -s INT 12 ./unityloader unity.toml
```

Expected plugin logs include `guide down touch=(433.0,307.0)` and a matching
Guide touch-up event. There must be no `skin message` entry.

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
