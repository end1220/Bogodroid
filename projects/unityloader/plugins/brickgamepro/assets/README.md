# BrickGame artwork and startup video

`splashvideo.mp4` is the user's latest desktop `BrickGame/splash.mp4`
(2026-10-16). It contains 8 seconds of 856×480, 24 fps HEVC/yuv420p video
and an AAC audio track. The deployed port is now named FatFishBrick (肥鱼方块).
The plugin requests silent playback, centered at 3/4 of the game window height,
preserving the video's aspect ratio. It loops for at least three seconds from
its first presentation and remains visible until the first scene is ready. The loader waits for the minimum before
Unity nativeResume, so scene animation does not run behind the minimum hold.

Prepare the H.264 runtime MP4 with FFmpeg on PATH:

```sh
python projects/unityloader/plugins/brickgamepro/tools/prepare-splash-video.py deploy/BrickGamePro/game --low-cpu --preset slow
```

This converts to H.264/yuv420p and removes audio. Output: `game/res/drawable/splashvideo.mp4`.
The low-CPU preset uses two-pass 400 kbps (440 kbps VBV maximum), constrained
baseline, no B frames, one reference and two slices for the decoder's two slice
workers. Original 24 fps timestamps and speed are retained; do not speed up this
already edited source again. Both handhelds use the same 3/4 height fraction.
The `--copy-video` option is only for an already compatible H.264 input.
The source MP4 is retained here so the runtime asset can be regenerated.
The obsolete frame sheet and BrickGame BMP conversion script have been removed.

Video decoding, timing, rendering and context handoff live in unityloader.
Only BrickGame's asset path, height fraction and scene-ready notification live
in its plugin. The runtime uses the loader's optional FFmpeg 58/56/5/3 libraries;
on the current Anbernic these are available in the system library path.
Other handhelds require compatible FFmpeg libraries, as described in AGENTS.md.
The animated path currently uses FAKE_EGL and requires an EGL pbuffer compatible
with SDL's configuration; unsupported drivers log a fallback to game-swap updates.

## Exit confirmation decoration

`exit-game.png` is the latest 571×464 source processed to transparent black
line art. White becomes transparent; gray coverage becomes partial black alpha:
`source_alpha * (255 - floor((299R + 587G + 114B) / 1000)) / 255`.
The original source is kept on the user's desktop; the processed PNG is embedded
by CMake. `exit_icons.cpp` creates one noninteractive RawImage of logical size
540×438.8 centered at (0,260), preserving 571:464. Original ExitMenu children
move down by 260 once per node, including FrameButton (the dashed Yes/No frame).
ExitMenu's own background stays fixed. A/B keep their original confirm/cancel
flow. The texture discards its CPU copy and temporary GC handle after ownership
passes to RawImage.

## Game Over decorations

`gameover-icon.png` is the user's 651×384 artwork converted to black RGBA.
White and the baked near-white checkerboard are transparent; gray edge coverage
becomes black with partial alpha. CMake embeds this processed PNG into the plugin.
`gameover_icons.cpp` creates one noninteractive RawImage, horizontally centered
within FinishMenu, eight logical units below the original action row's center.
Its 310.6×183.2 logical size is 15% larger than the first single-icon version,
preserving the image aspect. The edited transparent source PNG is embedded at
its full 651×384 resolution, with no additional transparency processing.
The four-icon sheet, quadrant UV crops and row-spacing logic have been removed.
All six original actions stay hidden; A still returns to the game selection menu.
The texture discards its CPU copy after loading and releases its temporary GC
handle once RawImage owns the reference. It lives with the first game scene.

## Boy atlas replacement

`tools/replace-boy-art.py INPUTS STAGED_DATA NEW_OUTPUT` processes Boy1, Boy2 and
gameover-icon together. It writes full-resolution transparent copies and both
lossless PNG and game-format ETC2 atlas previews. The white point is 235/255 to
remove the faint checkerboard baked into the icon's RGB input. Alpha is
`source_alpha * clamp(1 - luminance/235, 0, 1)`; RGB is always black.

Boy artwork is resampled from the full aligned source canvases with one common
LANCZOS scale and origin. Each packed rectangle is cut from that shared logical
canvas using the original sprite trim offset, with a two-pixel inset. Independently
cropping or centering the poses breaks alignment when the arms change. Sprite objects,
geometry, UVs, offsets, texture format, split sizes and all other serialized
objects stay byte-identical. Only ETC2 blocks touching the two Boy rectangles
are re-encoded, avoiding any quality loss elsewhere in the UI atlas or skins.
The tool backs up changed splits before activating the verified resource patch.
Copy the processed gameover-icon.png into this asset directory and rebuild.

For manually edited RGBA inputs, pass `--preserve-alpha` to retain their pixels
and save byte-identical source copies. Pass `--layout-manifest PREVIOUS_MANIFEST`
to reuse a corrected shared-canvas transform: edits to the silhouette do not
recenter or enlarge the artwork automatically. Legacy manifests with independent
per-pose crops/scales are rejected. `--boys-only` leaves the Game Over icon alone.
Only LANCZOS downsampling into the fixed atlas rectangles changes scaled pixels.
