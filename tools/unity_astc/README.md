# Unity bundle slimming pipeline

Shrink a Unity IL2CPP game's bundles to fit memory-constrained handhelds.
What matters is RAM (UMA: CPU+GPU shared), not disk size.

---

## TL;DR — pick one mode, run one command

Both modes use **`--block 6x6`** (safer than the default `8x8` for pixel
art) and let `retier_all.sh` auto-apply `--include-raw --compact
--packer original` for you.

### 🅰️ Mode A — Quality-first

> Compress **only big textures**, keep small ones original. Use when
> your `wsm.toml` has `textureMaxDim = 0` (runtime cap disabled).

```bash
./tools/unity_astc/retier_all.sh "$GAME_DIR" --keep-cap 768 --block 6x6
```

| What | Action |
|---|---|
| Long side ≤ 768 | **kept** (pixel icons / sprite tiles preserved at original quality) |
| Long side > 768 | **ASTC 6×6** (~9× compression) |
| Fonts | auto-detected, kept |
| LUT shape (short ≤32 or ratio ≥4:1) | kept |

### 🅱️ Mode B — Runtime-cap teamwork

> Compress **middle textures**, let the runtime cap handle the giants.
> Use when your `wsm.toml` has `textureMaxDim` set (e.g. 384/768).
> `--max-size` is the upper bound for ASTC (anything bigger goes to the
> runtime cap); see "Sweet spot" below for how to pick it.

```bash
./tools/unity_astc/retier_all.sh "$GAME_DIR" --max-size 1280 --block 8x8
```

| What | Action |
|---|---|
| Long side ≤ 256 (default `--keep-cap`) | kept |
| Long side in (256, 1280] | **ASTC 8×8** |
| Long side > 1280 | **kept** (runtime cap will box-downsample at upload) |
| Fonts | auto-detected, kept |

#### 🅱️➕ Mode B for pixel-art games — tiered blocks

> Pixel sprites' 1-px edges go soft under 8×8 blocks, but big backgrounds
> don't notice. Use a smaller block for the small ones, keep 8×8 for the
> big ones.

```bash
./tools/unity_astc/retier_all.sh "$GAME_DIR" --max-size 1280 --block 8x8 --block-small 6x6
```

| What | Action |
|---|---|
| Long side ≤ 256 | kept |
| Long side in (256, 768] (default `--small-threshold`) | **ASTC 6×6** (small sprites — protect 1-px edges) |
| Long side in (768, 1280] | **ASTC 8×8** (backgrounds — block artifacts not visible) |
| Long side > 1280 | kept (runtime cap) |

### Preview without writing

Append `--dry-run` to either command to see the per-bundle plan + total
RAM savings before committing.

```bash
./tools/unity_astc/retier_all.sh "$GAME_DIR" --keep-cap 768 --block 6x6 --dry-run
```

---

## Why two modes?

ASTC and the runtime cap compress in **different mathematical ways** —
they fit complementary size ranges:

| Compressor | Type | Best size range |
|---|---|---|
| ASTC 6×6 | **Relative** (fixed ~9× ratio) | small–medium textures |
| Runtime cap (e.g. 384) | **Absolute** (fixed pixel ceiling) | very large textures (4K → 0.5 MB regardless of input) |

So:

- **Mode A** uses ASTC for big textures because there's no runtime cap to do it.
- **Mode B** lets the runtime cap (which beats ASTC dramatically on huge inputs) handle big textures, and ASTC only fills the middle gap.

Fonts (R8/Alpha8) and LUT-shaped textures are silently kept in **both**
modes — never let block compression touch a lookup table or font atlas.

### Sweet spot for `--max-size` and `--block-small`

ASTC-vs-cap break-even is at `W ≈ cap × 3`:

| `wsm.toml textureMaxDim` | Recommended `--max-size` | Notes |
|---|---|---|
| 384 | 1024 | sweet spot ~1140 |
| **512** | **1280** | **sweet spot ~1500 — typical pixel-art port** |
| 768 | 2048 | sweet spot ~2300 |

For `--block-small`, 768 is a sane default threshold across most games —
sprites below that benefit from 6×6 edge fidelity, anything above is
big enough that 8×8 block artifacts vanish into the texel density.

---

## Optional follow-ups

### Audio → Streaming (frees RAM, zero quality loss)

```bash
./tools/unity_astc/retier_all.sh "$GAME_DIR" --audio --keep-cap 768 --block 6x6
```

### Video re-encode (only if game ships >720p cutscenes)

```bash
python3 tools/unity_astc/video_reencode.py "$BUNDLE.final"   # needs ffmpeg
```

Re-encodes cutscenes above `--max-height` (default 720) to the device
resolution. Skips videos already under the cap.

---

## Caveats (read once)

- **`--include-compressed` needs an ASTC-capable GPU.** Mali-G31+/Adreno/PowerVR 8+ yes; GLES2-era Mali-400/450 no — drop the flag there.
- **Never use `--packer lz4`.** UnityPy's lz4 preset sets the 0x80 padding flag (a 2020.3+ feature); pre-2020.3 players SIGBUS on it. The default `lz4hc` is what shipping packs use.
- **Fonts and small textures auto-kept** (TMP `* Atlas` naming + legacy Font refs + `--keep-cap`). Costs ~5% of savings, protects text/icon quality.
- **QA on device every time**: font rendering, map/inventory UI, particle-heavy scenes, area-transition audio. Add visibly broken textures to `--skip-file` and rerun.

---

## Single-bundle mode (alternative to `retier_all.sh`)

```bash
BUNDLE=/path/to/data.unity3d

# Equivalent of Mode A
python3 tools/unity_astc/astc_retier.py "$BUNDLE" --compact --jobs 10 \
    --include-raw --keep-cap 768 --block 6x6 -o "$BUNDLE.out"

# Equivalent of Mode B
python3 tools/unity_astc/astc_retier.py "$BUNDLE" --compact --jobs 10 \
    --include-raw --max-size 768 --block 6x6 -o "$BUNDLE.out"

# Then audio
python3 tools/unity_astc/audio_stream_patch.py "$BUNDLE.out" -o "$BUNDLE.final"
```

---

## Full flag reference

Every script also prints this via `--help`. **Bold = default.**

### `astc_retier.py` (textures)

| Flag | Default | What it does |
|---|---|---|
| `-o / --out` | `./output/<name>` | output path |
| `--block WxH` | **`8x8`** | target ASTC block for everything not matched by `--block-small` (`4x4`/`6x6`/`8x8`/...) — quality↔RAM dial |
| `--block-small WxH` | **unset** | optional smaller block for small sprites (long side ≤ `--small-threshold`). Set to `6x6` on pixel-art games to protect 1-px edges; unset = all textures use `--block` |
| `--small-threshold N` | **`768`** | long-side cutoff under which `--block-small` applies (only meaningful when `--block-small` set) |
| `--keep-cap N` | **`256`** | long side ≤ N → kept (**Mode A boundary**) |
| `--max-size N` | **`0`** (off) | long side > N → kept (**Mode B boundary**) |
| `--include-raw` | **off** | also convert uncompressed RGBA32/RGB24 |
| `--include-compressed` | **off** | also convert ETC2/DXT/BCn/PVRTC; needs ASTC-capable GPU |
| `--compact` | **off** | rebuild `.resS` dropping dead bytes — pass on real runs |
| `--skip NAME` / `--skip-file F` | — | leave named textures untouched (QA escape) |
| `--limit N` | all | process at most N textures (QA) |
| `--jobs N` | **`1`** | parallel encode threads (try cores−2) |
| `--packer` | **`lz4hc`** | bundle compression; `original` preserves shipped flags; never `lz4` |
| `--dry-run` | — | print plan, write nothing |
| `-q / --quiet` | — | less per-texture noise |

### `audio_stream_patch.py` (audio → Streaming; no re-encode)

| Flag | Default | What it does |
|---|---|---|
| `-o / --out` | `<bundle>.streamed` | output path |
| `--min-secs N` | **`10`** | only clips ≥ N seconds are flipped (keeps SFX latency) |
| `--skip NAME` / `--skip-file F` | — | clips to leave alone |
| `--limit N` | all | patch at most N clips (QA) |
| `--packer` | **`lz4hc`** | same semantics as astc_retier |
| `--dry-run` / `-q` | — | same semantics as astc_retier |

Tool never re-encodes audio by design — streaming alone frees most of
the RAM, and re-encoding for the last ~10 MB isn't worth the FSB5 rebuild.

### `video_reencode.py` (cutscenes; needs ffmpeg)

| Flag | Default | What it does |
|---|---|---|
| `--out-dir` | — | where rebuilt bundle + `.resource` go |
| `--max-height N` | **`720`** | re-encode only streams taller than N |
| `--crf N` | **`28`** | x264 quality (higher = smaller/worse) |
| `--preset P` | **`medium`** | x264 speed/size tradeoff |
| `--skip NAME` | — | videos to leave alone |
| `--passthrough` | — | QA mode: rebuild `.resource` offsets without ffmpeg |
| `--packer` / `--dry-run` / `-q` | **`lz4hc`** | same semantics as astc_retier |

### `retier_all.sh` (batch driver)

| Flag | Effect |
|---|---|
| `<dir>` | recurse for `*.bundle` + `data.unity3d` |
| `--audio` | chain `audio_stream_patch.py` (defaults) after the texture step |
| anything else | passed through to `astc_retier.py` |

**Forced per-bundle defaults**: `--include-raw --compact --packer original`.

Outputs land in `./output/<original filename>` (originals untouched);
after device QA copy them back over the originals.

---

## Reference numbers (Hollow Knight 1.5.78, Mode A pipeline)

| Asset | Stock | After |
|---|---|---|
| Textures (3602) | ASTC 4x4, 2105 MB VRAM | 8x8 except fonts/small, ~530 MB |
| Audio (1950 clips) | 149 MB resident | 32 MB resident |
| Bundle on disk | 695 MB | 367 MB |

---

## Other tools

- **`shrink_bundle.py`** — the *opposite* strategy: physically downscale
  resolutions instead of re-tiering blocks. Higher ceiling but must
  rewrite Sprite rects and can't fix TMP glyphs. Use only when neither
  mode is enough.
- **`extract_chars.py`** — count unique CJK codepoints across all
  containers (for prebake-font sizing, when needed).
- **`inspect_pixels.py`** — sample texture pixels + detect pixel-cell
  size to pick the right ASTC block.
- **`find_optimal_cap.py`** — search for the largest `textureMaxDim`
  that fits a memory budget.
