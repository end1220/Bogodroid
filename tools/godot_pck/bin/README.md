# bin/ — external tools

Not checked into git (see `../.gitignore`). Each developer pulls these
themselves with the commands below.

## GDRE Tools (Godot RE Tools)

Open-source godot pck extractor / recovery tool. Required for the
`extract` and `repack` pipeline steps.

- Upstream: https://github.com/GDRETools/gdsdecomp
- Used version: `v2.5.0-beta.5` (built on godot 4.7.dev, supports 4.x)
- License: MIT

Download (macOS):

```bash
gh release download --repo GDRETools/gdsdecomp v2.5.0-beta.5 \
  --pattern "*macos*" --dir tools/godot_pck/bin --clobber
unzip "tools/godot_pck/bin/GDRE_tools-*-macos.zip" -d tools/godot_pck/bin
```

Linux / Windows: same release page has `-linux.zip` / `-windows.zip`.

Quick test:

```bash
"tools/godot_pck/bin/Godot RE Tools.app/Contents/MacOS/Godot RE Tools" \
  --headless --gdre-version
```

## godot 4.5 editor (mono)

Needed by `reimport.sh` and `repack.sh`. Same binary we already use to
generate C# glue — see `../godot-sdl2/` instructions or download
official from https://godotengine.org/download/archive/4.5-stable .
