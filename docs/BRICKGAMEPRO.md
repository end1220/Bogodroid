# BrickGamePro Port Notes

## Current release (2026-10-20)

The handheld menu entry is **FatFishBrick (肥鱼方块)**. Android package
`com.perseusgames.brickgamepro`, plugin identity and save keys are unchanged.
The latest Anbernic endpoint is `172.16.7.42`, with runtime files under
`/mnt/mmc/Roms/ports/FatFishBrick`. Older IPs and captures below are historical.

- A is Tetris, B is Tank and G is BrickBreaker. Tank player movement is doubled;
  firing and enemy clocks are unchanged. G's held-A interval is 0.02 seconds
  instead of 0.01 seconds; release restores the game's normal interval.
- Tetris/TetrisFive Down steps immediately, waits 100 ms, then repeats every
  30 ms for the current piece. A/Y rotation calls Rotate on the Unity thread.
- Guide directly calls PressExitBtn: in gameplay it returns to selection;
  from selection it opens Exit Game. Start directly calls PressStartPauseGame.
  Neither uses fixed touch coordinates. Select alone is unmapped;
  Start+Select retains the loader exit hotkey.
- Game Over A returns to selection. Exit Game A confirms and B cancels.
  HUD and exit-menu font density is corrected. Original square cells are kept.
- Silent splash video fits within 3/4 of window height and remains visible for
  at least three seconds from first presentation, then until the scene is ready.
  Native library preparation overlaps playback; Unity nativeResume waits for
  the minimum so the opening game animation does not run during that hold.
- A power-key tap pauses the nativeRender loop at a frame boundary, enters
  kernel mem standby after key release and resumes after wake. The user
  confirmed physical standby and final Guide behavior on the handheld.

The final stripped Release build has LOG/TRACE/VERBOSE/IL2CPP_TRACE OFF,
`JNIVM_ENABLE_RETURN_NON_ZERO=OFF` and JNI class-registration DEBUG ON.
SDL/GL/FFmpeg native messages and fatal diagnostics may still be emitted.
The latest 720x720 container replay exited normally with four native output
lines and zero `[BD-*]` lines.

| Runtime file | SHA256 |
| --- | --- |
| `unityloader` | `dfb11c522151b320bdbad84d47b00463b0ca5025bb2cbbb0482ea6c5c92f5293` |
| `unityloader.d/brickgamepro.so` | `45664d72c934b24e38741218cbbe6802f8b046a3a4963eb08aeedd5dee3af9a4` |

These files were synchronized to the latest handheld and both extracted
Desktop distributions under `C:/Users/Administrator/Desktop/FatFishBrick`,
including their SHA256SUMS. Runtime staging and captures are ignored under
`deploy/BrickGamePro/`; source commits do not include game resources or binaries.
Previous validation records below describe earlier builds, not the current
runtime hashes or endpoints.

See [startup/video analysis](BRICKGAMEPRO_SPLASH_ANALYSIS.md),
[UI and artwork](BRICKGAMEPRO_UI_ANALYSIS.md),
[Down/rotation evidence](BRICKGAMEPRO_DOWN_ANALYSIS.md), and
[oddmar merge assessment](BRICKGAME_ODDMAR_MERGE.md).

## Source cleanup verification (2026-10-16)

The cleanup removes unused ExitMenu getter bindings and its unused read helper,
as well as the redundant startup frame-upload flag. The retired BrickGame BMP
preparation script is deleted. Generic image/animation/video APIs remain reusable
loader capabilities; all BrickGame constants, artwork and IL2CPP rules stay in
its plugin. Asset tooling and processed UI PNGs are tracked with that plugin;
game resource packages, saves and local deployment captures remain ignored.

A fresh stripped Release loader/plugin build with every log switch OFF passed
Guide → B → Guide → A in ARM64/QEMU, exiting normally after 35.8 seconds.
The final screenshot retains the corrected image aspect and Yes/No border;
no crash markers or `[BD-*]` lines were present. Existing rotation and Game Over
diagnostic evidence passed their validators again; Python and shell syntax and
`git diff --check` passed. Evidence and source-cleanup binaries are kept in
`deploy/BrickGamePro/source-cleanup-validation/`, separately from deployed files.
This source-cleanup rebuild was not deployed to the handhelds.

## Branch Status

- Branch: `BrickGame`.
- Shared history includes `484072d` on `oddmar`. The later Oddmar commit
  `68d0abc` is not merged here; this cleanup does not merge other branches.
- Current workspace: `D:\Locke\gitee\Bogodroid-brickgame`.
- The BrickGame linked worktree was moved here on October 12, 2026. Use this
  directory for builds, container staging, commits, and pushes.
- BrickGamePro-specific behavior is isolated in `projects/unityloader/plugins/brickgamepro/`.
- Shared loader work includes startup video decoding/presentation and context
  handoff, plugin touch injection, viewport/display support, and IL2CPP helpers.
  Android input compatibility remains in its existing common bridge.
- Staging directory: `deploy/BrickGamePro/` is local-only and ignored by git.

## Runtime Layout

Runtime config is `configs/brickgamepro.toml`, deployed as `unity.toml`.
Required files include `unityloader`, `unityloader.d/brickgamepro.so`, the game
resources, controller database and `game/res/drawable/splashvideo.mp4`.
`init_config` changes cwd to `game/`; Android external/files paths are
`../conf/`, cache is `../cache/`, and source/package/OBB paths are `./`.

The Anbernic launcher source is `scripts/brickgamepro_launch.sh`, deployed beside
its `FatFishBrick/` directory as `FatFishBrick.sh`. It resolves that directory
relative to itself, creates config/cache folders, selects Pulse/ALSA, and runs
in the foreground with output discarded. Trimui uses the installed PortMaster
launcher and its separate `Data/ports` layout. Logging is a compile-time choice;
launcher redirection does not enable or disable loader logs.

Do not deploy Oddmar plugins for this game. Startup video uses the loader's
optional FFmpeg runtime; current Anbernic and Trimui provide compatible system
libraries. Other installations must configure their own compatible FFmpeg path,
as described in AGENTS.md, without depending on another port directory.

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
   2.2x scale and a calibrated vertical anchored position.

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
`kWorldCameraOffsetX=0.0`, and `kWorldCameraOffsetY=10.55`. The camera offset
was derived from the previous `(+2.5, -4.0)` value plus a requested visible
correction of about `+2.5` cells right and `-13.75` cells down, followed by
the real-device request to move the game area down another `0.25` cell.
The initial fine adjustment lowered the camera offset from `10.60` to `10.50`,
moving the entire dynamic playfield up `0.10` cell. The latest adjustment raises
it to `10.55`, moving the dynamic playfield down `0.05` cell from that version.
The background UI remains fixed throughout these adjustments.

The original casing consists of `CasingCenter`, `Casing_Left`, and
`Casing_Right` under AppContent. In the 640×480 container, its outer extent was
pixels 23 through 616, leaving a black strip at each side. The plugin extends
only the two decorative side RectTransforms outwards to the current Canvas
width (plus one physical pixel of overscan). Their inner edges remain fixed;
the center casing, Grid, HUD, AppContent scale and camera zoom are unchanged.
This fills the sides without stretching the square gameplay cells. Side artwork
can widen slightly; labels remain at their original size and centered on each side.

Container validation (2026-10-14): Blue casing now covers x=0 through 639
at y=0,40,240,470. The checked HUD and background-grid patches are pixel-identical
to the previous version; matching static bottom blocks gives a +1px vertical
shift (requested +0.05 cell is 1.056px). Guide → B cancel → Guide → A quit exits
with code 0 and saves preferences. Evidence is in ignored staging
`deploy/BrickGamePro/casing-validation/`. The plugin is deployed to Anbernic
`172.16.6.250`; all 145 runtime file hashes pass. Previous plugin and manifest
are saved in `backup-casing-20261014-2040/`. Actual hardware appearance and the
other skins still require visual confirmation; no remote game launch was used.

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

The plugin resolves each button's `RectTransform` at runtime and injects at its
actual screen-space center. It does not use fixed 640x480 button coordinates,
so the same input path works on 1280x720 devices. Overlay canvases use their
world coordinates directly; camera canvases use `Camera.WorldToScreenPoint`.
The `RectTransformUtility.WorldToScreenPoint` path is intentionally not used
because that method is absent from this game's IL2CPP metadata.

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

- The original Perseus Games splash is an Android Java View displaying
  `res/drawable/unity_static_splash.png`. The port now uses the user-supplied
  silent video at two-thirds window height during initialization, then releases
  its decoder, frame buffers, file, GL objects and independent context at scene
  handoff. Shared FFmpeg runtime libraries remain mapped for the process lifetime.
  See [splash analysis](BRICKGAMEPRO_SPLASH_ANALYSIS.md).
- D-pad, A/Y, Start/Pause, X/Sound, and Guide/Menu Exit Game are routed through
  the BrickGamePro plugin. Select skin switching is disabled.
- In ExitMenu, A confirms quitting and B cancels; elsewhere B is unmapped.
  The previous B pause experiment has been removed.
- The Canvas/camera layout is visually correct on the tested 4:3 TrimUI panel
  with `kCanvasPositionY=-1190` and current `kWorldCameraOffsetY=10.55`,
  following the requested `0.10` cell upward then `0.05` cell downward adjustments.
- The Tetris Down fix is built and verified in the ARM64 container. IL2CPP
  disassembly identified shared vertical timers and a held-Down branch that
  bypasses the repeat timer. The plugin now preserves short taps and schedules
  original single-step moves at 100 ms / 30 ms. Container checks cover release,
  pause/resume, movement/rotation combinations, held-key spawn gating, game over, TetrisFive,
  and Tank fallback. Device feedback confirmed the original 200 ms / 90 ms fix
  improved Down feel; the user subsequently requested 100 ms / 30 ms to match
  the original Tetris repeat interval.
  Single-brick, bomb, and line-clear scenarios still need dedicated testing. See
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

Build in `GlES_Dev`. Its checkout is a separate copy, so sync changed sources,
helper headers and assets before building. For example, from the host:

```powershell
docker cp projects/unityloader/. GlES_Dev:/workspace/Bogodroid/projects/unityloader/
docker cp thunks/egl_sdl/egl_sdl.cpp GlES_Dev:/workspace/Bogodroid/thunks/egl_sdl/egl_sdl.cpp
```

Inside `/workspace/Bogodroid`, explicitly configure the cached release build:

```sh
cmake -S . -B build-device-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DBD_ENABLE_LOG=OFF -DBD_ENABLE_TRACE=OFF -DBD_ENABLE_VERBOSE=OFF \
  -DIL2CPP_TRACE=OFF -DJNIVM_ENABLE_RETURN_NON_ZERO=OFF
cmake --build build-device-release \
  --target unityloader bogodroid_unityloader_plugin_brickgamepro -j4
strip build-device-release/unityloader build-device-release/unityloader.d/brickgamepro.so
```

Copy both outputs into `/game/BrickGamePro/` for release smoke verification:

```sh
cp build-device-release/unityloader /game/BrickGamePro/unityloader
cp build-device-release/unityloader.d/brickgamepro.so /game/BrickGamePro/unityloader.d/brickgamepro.so
bash scripts/container-brickgamepro-ui-matrix.sh exit
```

Check its runner for a normal `exit=0`, inspect the dialog screenshot and verify
that `log.txt` has no `[BD-*]` lines. An `exit=124` only means the time limit;
it does not prove a normal exit. UI, Down and rotation replays share Xvfb and
log files, so run those suites sequentially.

Log-based diagnostic validators require a separate build with
`BD_ENABLE_LOG=ON` and the corresponding plugin trace flags. Preserve the
LOG=OFF release artifacts separately. The diagnostic UI and rotation tools are:

```sh
bash scripts/container-brickgamepro-ui-matrix.sh
python3 scripts/verify-brickgamepro-ui.py /game/BrickGamePro
bash scripts/container-brickgamepro-rotate-matrix.sh
```

The rotation verifier expects the four case directories `tetris`, `five`,
`pause`, `quick`; copy each `seq-rotate-combo-CASE` into a common validation root
under that case name, then run `verify-brickgamepro-rotate.py ROOT`.

Prepare runtime video using the plugin's `tools/prepare-splash-video.py` as
shown in [the asset instructions](../projects/unityloader/plugins/brickgamepro/assets/README.md).
Stage files locally, deploy with Dropbeak chunk/force/verify, then verify the
whole remote SHA256SUMS. Keep backup files outside `unityloader.d/` so they
cannot be loaded as extra plugins. Build and source cleanup do not automatically
refresh historical deployment archives or replace the user-tested handheld files.


### Tank speed and splash minimum

Tank player movement now advances its independent `moveTimer` at twice the
original rate. Firing and enemy clocks retain their original speed. Set
`BRICKGAME_TANK_SPEED_FIX=0` for a baseline comparison. Run
`scripts/container-brickgamepro-tank-speed.sh` for sequential four-direction
replays; `BRICKGAME_DOWN_TRACE=1` logs `TANK move` timer resets.

Startup videos remain visible for at least 3000 ms from the first presentation,
and longer until the scene is ready. Native library preparation proceeds during
the video; immediately before Unity nativeResume, the loader waits out any
remaining minimum time while the video continues. Scene updates therefore start
after the minimum, preventing its opening animation from running behind the
extra splash hold. Background and game-swap fallback both support this gate.

The original square cell artwork is retained; the experimental heart artwork
was reverted locally and on the handheld at the user's request.

Container validation (2026-10-19, QEMU aarch64 + llvmpipe): median direction
repeat intervals baseline/modified were right 207/103 ms, left 201/96 ms,
up 215/122 ms and down 208/112 ms. Frame quantization accounts for variation
from the 2x timer rate. A separate Up-first baseline avoids a player death in
the longer baseline replay. Startup durations were 4.2-4.7 seconds; an isolated
container C++ harness using the actual dismissal predicate checked the 2999/3000
ms boundary, unready scene and SDL tick wrap. The forced-ready plugin probe was
container-only and restored afterward. Exit menu replay completed with exit=0.
Local artifacts and evidence: `deploy/BrickGamePro/tank-speed-validation/`;
`SHA256SUMS` records the staged runtime files.
`verify-brickgamepro-tank-speed.py ROOT` checks the exported `baseline`, `fast`,
`up-baseline`, `splash-early-ready`, and `exit` directories. The final version was deployed and tested on the handheld.

2026-10-19 follow-up: the heart artwork was subsequently reverted. The resume-gate
container harness checks fast/slow loading, independent/fallback animation and
missing splash. Evidence is under `deploy/BrickGamePro/heart-full-v3/`.

Follow-up full-container trace: minimum wait began at +1729 ms, completed at
+4375 ms before nativeResume; GUIManager.Start completed afterward and the first
scene handoff followed at +8179 ms. The first presentation was about +1375 ms.
The experimental heart artwork was reverted; the exit replay completed with exit=0.

### G 号打砖块 A 键加速

G 对应 BrickBreaker。插件在 BrickBreakerManager.StartMoveBallFast 返回后，将 timeMoveBall 从 0.01 秒加倍为 0.02 秒，使目标加速频率降低 50%。松开 A 仍由游戏原方法恢复普通速度；每次按下先重设间隔，不会叠加倍率。

GlES_Dev 容器对照：15 秒和 19 秒各按住 A 两秒，基线两次均为 0.010000→0.010000，修改版两次均为 0.010000→0.020000。BRICKGAME_G_SPEED_FIX=0 可禁用，BRICKGAME_DOWN_TRACE=1 输出间隔。实际移动频率受帧率限制。

### Exit Game 文本清晰度

`discover_control_rects` 沿层级树追踪 ExitMenu 子节点，并将其动态字体纳入 `Text.get_pixelsPerUnit` 的 2.2×补偿。涵盖标题、Yes/No 和 FrameButton 装饰虚线；不改变字号、位置或面板布局。容器退出菜单验证前后 PPU 从 0.188 增至 0.413，退出回归 B 取消及 A 确认均通过（exit=0）。

### 电源键待机

unityloader 在 SDL 电源键按下时请求暂停，在 nativeRender 帧边界阻塞主循环；物理按键释放后由输入线程向 /sys/power/state 写入 mem。内核待机返回后解除阻塞。电源键不再作为 Android 游戏输入注入，唤醒后短暂过滤重复电源键事件。打不开或写入失败时记录错误并恢复主循环；按键未释放或主循环未响应也有超时恢复。

容器脚本 scripts/container-unityloader-power-standby.sh 使用 BD_POWER_REPLAY、BD_POWER_STATE_PATH 和 BD_POWER_TEST_HOLD_MS 模拟电源键与内核待机返回，避免挂起宿主机。实测 1616ms 模拟待机期间 nativeRender 帧计数始终为 181；另一次恢复后确认退出成功（exit=0）。容器不能验证物理熄屏、内核真实挂起和硬件唤醒；用户已在掌机实测确认待机正常。


### Guide / Start independent of screen aspect ratio (2026-10-20)

Guide and Start no longer inject fixed 640x480 touch coordinates. SDL queues
atomic requests consumed by GUIManager.Update on the Unity thread. Guide invokes
GUIManager.PressExitBtn directly, preserving the original behavior: return to
selection while playing, show Exit Game from selection. Start invokes the original
PressStartPauseGame action. Select
alone remains unmapped and Start+Select remains the loader exit hotkey. Direction
and face-button mappings are unchanged.


The final Guide behavior passed a 720x720 Tetris replay and handheld testing:
first Guide returns to selection, second Guide opens Exit Game, B cancels and
A quits. Release distributions use BD_ENABLE_LOG=OFF, BD_ENABLE_TRACE=OFF,
BD_ENABLE_VERBOSE=OFF, IL2CPP_TRACE=OFF and JNIVM_ENABLE_RETURN_NON_ZERO=OFF.
SDL/EGL or Unity native output and fatal diagnostics may still appear.

Release verification: the LOG=OFF 720x720 normal-button replay completed with
exit=0 and four native SDL/GL/FFmpeg output lines, with no [BD-*] messages. A
separate mocked standby run wrote mem to its test state file. The stripped
loader and plugin were synchronized to both Desktop FatFishBrick distributions
and handheld 172.16.7.42; remote SHA256 values matched local staging.

## Separate casing character (2026-10-21)

The Blue atlas now uses the supplied `BlueNew2.png` (versioned as
`assets/blue-casing.png`). `tools/replace-blue-skin.py` replaces only texture
path ID 16's ETC2_RGBA8 payload in `sharedassets0.assets`: split11 and split12
change, with the original split lengths, all Sprite metadata and every other
serialized object preserved. The generated manifest and previous split files
are in `build-trimui-1280x720/blue-separated/`.

The supplied 244x493 `女孩.png` is embedded as `assets/casing-girl.png` in the
plugin. `casing_art.cpp` creates one noninteractive RawImage under the root
Canvas, after the casing branches. Its left edge follows CasingCenter's right
world corner converted back into root Canvas coordinates, with a 0.4%-of-height
overlap, an additional left shift of 11/244 of the rendered character width
(11 source-art pixels), and a 1.5% bottom inset. This keeps the character against the game panel
on both 4:3 and wide screens, instead of against the physical screen edge. Height is 45% of Canvas height; width is
height multiplied by 244/493. This deliberately avoids inheriting the stretched
casing rectangle. Geometry updates only when Canvas dimensions change. The
managed texture releases its CPU pixel copy and temporary GC root after loading.

Container validation used the current ARM64 loader and rebuilt plugin with
Release, LOG ON, TRACE/VERBOSE/IL2CPP_TRACE OFF, and explicitly
JNIVM_ENABLE_RETURN_NON_ZERO OFF. Xvfb screenshots at 1280x720 and 640x480 show
an unstretched character fully within the right casing, at respectively 324 and
216 pixels high. Files: `build-trimui-1280x720/blue-separated/1280x720.png` and
`640x480.png`. The panel-aligned revision is captured as
`panel-1280x720.png` and `panel-640x480.png` in the same directory. Capture the X11 root window for visual verification: this
container's GL readback dump is black even when the actual window is rendered.
No handheld deployment was performed.

## Splash RGB upload compatibility (2026-10-22)

The startup decoder outputs packed RGB24 and uploads it with `GL_RGB`.
Container FFmpeg 4.2 and the tested TrimUI runtime both returned zero rows for
YUV420P-to-RGBA conversion; RGB24 returned the complete frame. Keep the decoder
buffer size, row stride, initial texture format and subsequent uploads consistent.
This avoids extra runtime libraries. TrimUI and Anbernic both passed handheld
playback tests with the current 752x560 H.264 startup video.

Release deployment for this fix explicitly disables `BD_ENABLE_LOG`, TRACE,
VERBOSE and IL2CPP_TRACE; `JNIVM_ENABLE_RETURN_NON_ZERO` stays OFF. Fatal error
diagnostics remain independent of these switches.
