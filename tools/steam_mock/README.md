# steam_mock — minimal Steamworks SDK stub for arm64 Linux

A `libsteam_api64.so` shim that exports every Steamworks 1.59+ entry point
expected by a typical .NET game (Steamworks.NET via `[DllImport]`) and
returns sane defaults. Lets a Steam-dependent game initialize on a
device that has no Steam client installed at all (PortMaster handhelds).

The shim does NOT implement Steam functionality — `Init` returns "OK",
interface getters return a shared non-NULL handle so wrapper null-checks
pass, individual methods return reasonable empties ("English" for
language, 0 controllers, no overlay, etc.). The game then continues into
its offline / no-Steam code path.

## Build

```bash
# 1. Clone gbe_fork (Goldberg Best Edition) once, to mine the canonical
#    Steam SDK 1.59+ flat-C surface from its dll/flat.cpp + dll/dll.cpp.
git clone --depth 1 https://github.com/Detanup01/gbe_fork.git /tmp/gbe_fork

# 2. Parse → emit steam_mock_gen.c (~1200 C stubs).
python3 gen_stubs.py /tmp/gbe_fork/dll

# 3. Cross-compile arm64 .so.
zig cc -target aarch64-linux-gnu -shared -fPIC -O2 -Wl,--strip-all \
       steam_mock_gen.c -o libsteam_api64.so
```

Total cycle: a few seconds. Deploy `libsteam_api64.so` next to the game
executable and into the .NET app's `data_*` dir (P/Invoke search order
includes both).

## Files

- `gen_stubs.py`        — parser + emitter. Edit `OVERRIDES` to tune the
                          return value of a specific method when the game
                          insists on something other than the default.
- `steam_mock_gen.c`    — generated C (~80 KB, gitignored).
- `libsteam_api64.so`   — compiled arm64 .so (~140 KB, gitignored).

## When a game still crashes after this stub

1. Grep the game's stderr for `EntryPointNotFoundException` — that's a
   missing export. Newer Steam SDK function not in gbe_fork's flat.cpp?
   Add it to `gen_stubs.py` or upgrade gbe_fork checkout.
2. Steamworks.NET's `CSteamAPIContext.Init` calls ~15 `ISteamClient_GetISteam*`
   and treats any IntPtr.Zero as failure → make sure `OVERRIDES` returns
   our dummy interface for those. (Already covered for the canonical 15.)
3. The game polls `ISteamInput_GetConnectedControllers` per frame; default
   `return 0` keeps it quiet but harmless.
