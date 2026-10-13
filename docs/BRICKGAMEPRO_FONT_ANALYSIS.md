# BrickGamePro Font Rendering

## Diagnosis (2026-10-12)

The APK embeds dynamic TrueType fonts (`digital-7`, `digital-7 (italic)`,
`consolaz_0`). Their serialized font textures have zero dimensions: Unity
generates glyph atlases at runtime. The deployed resources were not downscaled.

Runtime inspection at 640x480 reports root Canvas render mode **1** (Screen
Space Camera), `scaleFactor=0.1875`, and HUD `fontSize=60` with
`Text.pixelsPerUnit=0.1875`. The serialized root transform used for camera
calibration does not establish the runtime Canvas render mode.

The port scales AppContent by 2.2. Unity's dynamic Text accessor still returns
the root Canvas scale factor. As a result, the HUD requests approximately
11-pixel glyphs and magnifies them to the displayed size. Bilinear sampling
then makes the score labels and numbers visibly soft.

IL2CPP disassembly confirms:

- `Text.get_pixelsPerUnit`: `0x6C2B38`; dynamic fonts return Canvas.scaleFactor.
- `Text.GetGenerationSettings`: `0x6C2F1C`; calls that accessor at `0x6C30AC`
  and writes the result into TextGenerationSettings.scaleFactor at `0x6C3208`.
- `Text.OnPopulateMesh`: `0x6C33DC`; calls the same accessor at `0x6C3588`
  and computes its reciprocal at `0x6C370C` for generated mesh coordinates.

## Fix

The title plugin hooks `Text.get_pixelsPerUnit` and multiplies the original
result by `kCanvasScale` for dynamic Text components directly under MainGUI
and FinishMenu. This increases rasterization resolution and uses Unity's
matching inverse mesh scaling, preserving the nominal layout. The 640x480
HUD now reports PPU 0.4125 and requests approximately 25-pixel glyphs.

Components are identified during GUIManager.Start on the Unity thread and
marked dirty through Graphic.SetAllDirty. Hidden controls retain their own
compensated layout. Original fonts and texture filtering remain in use.

`BRICKGAME_FONT_FIX=0` disables the correction for comparison.
`BRICKGAME_FONT_TRACE=1` logs component sizes and PPU values.

## Validation

The Release ARM64 plugin was built and run in GlES_Dev at 640x480. A/B captures
show sharper SCORE, HI-SCORE, GOAL, LEVEL and SPEED text. Game Over and score
text were also inspected without clipping or overlap. Native captures and
logs are in ignored local staging `deploy/BrickGamePro/font-validation/`;
`font-comparison.png` contains equally scaled nearest-neighbor crops, with
the original rendering on the left and corrected rendering on the right.
The runs use random pieces, so preview shapes differ.

These captures verify the container rendering improvement. Android-device
pixel equivalence and the physical handheld display still require user
comparison after deployment. Other panel resolutions were not screenshot
tested in this run.
