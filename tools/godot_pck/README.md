# godot_pck — recompress textures inside a godot 4.x pck

Toolkit for porting godot games to handhelds whose GPU does not support
the textures the desktop build was shipped with — typically `BPTC`/`S3TC`
(desktop GPU formats) on a Mali / Adreno mobile GPU that only knows
`ETC2`/`ASTC`.

Without re-encoding, godot falls back to CPU decoding to RGBA8 → ~16 MB
per 2048² atlas in RAM, and a card-game-sized pack of atlases easily
exhausts 1 GB devices via the OOM killer.

## Layout

```
tools/godot_pck/
├── extract.sh            \
├── patch_project.py       |  generic atomics — no game-specific logic here
├── reimport.sh            |
├── strip_non_astc.py      |
├── repack.sh             /
├── bin/                  external tools (gitignored, see bin/README.md)
├── work/                 scratch tree, one subdir per game (gitignored)
│   └── <game>/
│       ├── recovered/    intermediate (extract output, safe to delete)
│       └── output.pck    final artifact — scp THIS to the device
└── patches/              per-game overlays and orchestrators
    └── <game>/
        ├── build.sh      one-shot end-to-end runner for this game
        ├── apply.sh      just the overlay step (called by build.sh)
        ├── overlay/      files that rsync into recovered/
        └── README.md     why each file in overlay/ exists
```

## One-shot for a known game

```bash
patches/sts2/build.sh /path/to/SlayTheSpire2.pck
# ⇒ work/sts2/output.pck
```

## Pipeline (what `patches/<game>/build.sh` runs internally)

```
INPUT_PCK (anywhere on disk, untouched throughout)
        │
        ▼  extract.sh                gdre_tools --recover
work/<game>/recovered/
        │
        ▼  patch_project.py          [rendering] vram_compression → mobile
        ▼  force_vram_compress.py    every compress/mode 0 → 2
work/<game>/recovered/
        │
        ▼  reimport.sh               godot --headless --import (~25 min)
work/<game>/recovered/.godot/imported/*.{astc,bptc,etc2,s3tc}.ctex
        │
        ▼  strip_non_astc.py         drop bptc/etc2/s3tc — keep astc
        │
        ▼  patches/<game>/apply.sh   rsync overlay/ + edit project.godot
work/<game>/recovered/
        │
        ▼  build merged overlay tree (inline in build.sh)
work/<game>/overlay/                  ctex + .import + project.godot
                                      + .godot/extension_list.cfg
                                      + patches/<game>/overlay/* (gdext, stubs)
        │
        ▼  apply_overlay.py          gdre_tools --pck-patch onto INPUT_PCK
work/<game>/output.pck                ⇐ scp THIS to the device
```

The key choice is `--pck-patch` rather than `--pck-create`: the original
pck is the base and we only layer in what changed. Anything the host
godot editor can't regenerate (spine `.spskel`/`.spatlas`, FMOD `.bank`,
compiled `.gdc`, …) is inherited unchanged from the original. The output
ends up bigger than the input by roughly the size of the new ASTC ctex
but keeps every vendor-binary intact.

## Prerequisites

1. `bin/` external tools — see `bin/README.md` for download commands.
2. godot 4.5 mono editor (provided in `bin/`, used by `reimport.sh`).
3. ~10 GB free disk space per game in `work/<game>/`.
4. zsh / bash and Python 3.

## Local-use disclaimer

This unpacks proprietary game data. Use only for your own legitimately
owned copy and never redistribute the recovered project, the modified
`.import` files, or the repacked pck.

## Files

- `extract.sh`         — gdre `--recover` on a pck
- `patch_project.py`   — set `[rendering] textures/vram_compression/*` for mobile
- `reimport.sh`        — `godot --headless --import` to regenerate `.ctex`
- `strip_non_astc.py`  — drop `.bptc.ctex` / `.s3tc.ctex` / `.etc2.ctex` (keep only ASTC)
- `strip_sources.py`   — drop source `.png` / `.jpg` left behind by GDRE (runtime reads `.ctex`)
- `repack.sh`          — gdre `--pck-create` from the recovered dir
- `bin/`               — external tool binaries (gitignored, see `bin/README.md`)
- `work/`              — scratch trees (gitignored)
- `patches/<game>/`    — per-game overlay + orchestrator

## Adding a new game

1. Copy `patches/sts2/` to `patches/<newgame>/`.
2. Replace the files under `overlay/` with whatever that game needs
   (vendor gdextension entries, autoload stubs, …).
3. Adjust `apply.sh` if the per-game `project.godot` surgery differs.
4. Run `patches/<newgame>/build.sh /path/to/<newgame>.pck` and ship
   the resulting `work/<newgame>/output.pck`.

The generic atomics MUST stay game-agnostic. Any "if game == sts2"
in extract/patch_project/reimport/strip_non_astc/repack belongs in
the game's `apply.sh` instead.
