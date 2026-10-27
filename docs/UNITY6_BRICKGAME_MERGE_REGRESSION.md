# Unity 6 BrickGame Merge Regression

2026-10-27, branch `BrickGame`, merge commit `ef8d34a` (parents `408dc0d` and
`edb3ea7`). The merge fixes below were built and exercised in the already
running `GlES_Dev` aarch64 container. Tests ran against isolated copies of the
games, not handheld save data.

## Build

Source: `/workspace/Bogodroid`; build directory:
`/workspace/Bogodroid/build-device-release`.

Configured as Release with `BD_ENABLE_LOG=ON`,
`BD_ENABLE_TRACE=OFF`, `BD_ENABLE_VERBOSE=OFF`, `IL2CPP_TRACE=OFF`, and
`JNIVM_ENABLE_RETURN_NON_ZERO=OFF`. The explicit last setting avoids the
documented CMake cache trap. All 86 Ninja targets completed, including
`unityloader` and both game plugins.

| Deployed file | SHA-256 |
|---|---|
| `unityloader` | `4c7cb948e6c4d0488d2c9a1070973e3d60f54f6fcd46bab071bdd7183a9ca9e5` |
| `brickgamepro.so` | `e02d82f46b9eae40a7f520f61bc58f48c1835aaea6e8fc62c5b9b20aeb8c788b` |
| `spacesquids.so` | `5247c7a0ae22e72b7e1522b98adcc1586ebccdb0af236ae066b82b2a7ad2d563` |

## Merge Fixes

- Closed the misplaced `graphics` namespace and removed duplicate Android
  service and OBB declarations/definitions.
- Closed `normalize_jni_sig` and `JNIBridgeProxy::onFrameAvailable`, and
  removed the duplicate `DisplayListener` base.
- Removed the local fake `Choreographer` definition, which conflicted with the
  real JNI stub and caused an ODR/LTO hazard.
- Kept the one-argument `Class.forName` name normalization and Unity 6 JNI
  initialization path with its legacy fallback.
- Updated the BrickGamePro verifier to recognize direct GUI calls for Start
  and Guide and A's rotate edge, rather than expecting touch injection for
  those controls.

## BrickGamePro

Ran `scripts/container-brickgamepro-ui-matrix.sh` in the container. The three
`finish`, `buttons`, and `exit` cases passed
`scripts/verify-brickgamepro-ui.py`:

- Natural Game Over, A closes the finish panel, Start begins a new game, and
  Down moves/steps a piece.
- Start, Guide, X, A rotation, D-pad movement, and Select behavior were
  exercised. UI visibility checks consistently found 19 hidden renderers,
  zero invalid alpha values, and six hidden finish actions.
- B cancels the exit dialog; A confirms it. The game exits with code 0 and
  saves preferences.

## Space Squids

Ran `scripts/test-spacesquids-cursor.sh` against `/game/SpaceSquids-cursor`
with `cursor-640.toml`, Xvfb, llvmpipe, and dummy audio. Each run used a fresh
results directory.

- `merge-guide` opened Options, activated the current audio slider, and
  returned to the menu.
- `merge-shop` scrolled through all 11 ship cards and returned to the first
  card and menu.
- `merge-combat-retry` started a game, moved with the left axis, fired with A,
  used X twice, paused with Start, and resumed with B. Trace output confirmed
  player movement and `rocket ammo=3->2->1`; movement returned to neutral on
  release. The game later reached Game Over naturally.
- `merge-transition` held X across menu, pause, and resume; that held input did
  not fire a rocket during the transition. The run reached Game Over without
  a crash or binding failure.

These runs ended on the harness timeout (`exit=124`), as intended by the test
script; they do not test Space Squids' normal save/exit path. BrickGamePro's
exit case did finish normally. Audio was dummy output in the container.

The container's BrickGamePro OpenGL frame dump was black (`mean R=0`) even
though its captured window screenshots showed the game correctly. Visual
checks of window captures confirmed both games rendered gameplay and their UI
dialogs; this OpenGL dump path is not treated as visual evidence.

## Repeat Commands

```sh
cd /workspace/Bogodroid
bash scripts/container-brickgamepro-ui-matrix.sh
python3 scripts/verify-brickgamepro-ui.py /game/BrickGamePro

CASE=merge-combat-retry GAME_CONFIG=cursor-640.toml SECS=58 \
SPACESQUIDS_TRACE=1 \
BD_PAD_AXIS_REPLAY='24500:leftx:20000,27000:leftx:0' \
BD_PAD_REPLAY='18000:a,22000:a:1500,28000:x:2000,31500:start,34500:b,38000:x:2000' \
bash scripts/test-spacesquids-cursor.sh
```
