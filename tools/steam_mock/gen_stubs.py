#!/usr/bin/env python3
"""
Generate steam_mock_gen.c from gbe_fork's dll/flat.cpp.

The C symbol name is what matters for the dynamic linker; the original
arg list is irrelevant (arm64 cdecl puts args in x0-x7 and we just don't
read them, returning a sane default in x0/v0). So every stub becomes a
zero-arg function with a default return value picked by return type.

Manual overrides cover the dozen-or-so calls a typical game card-game
makes early enough that returning the absolute default would surprise it:
IsSteamRunning, GetCurrentGameLanguage, GetPersonaName, etc.
"""

import re
import sys
from pathlib import Path

GBE_DLL = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/gbe_fork/dll")
OUT_C = Path(__file__).parent / "steam_mock_gen.c"
# flat.cpp = 1093 ISteam<X>_<Method> flat-C wrappers; dll.cpp = ~90 top-level
# SteamAPI_/SteamInternal_ entries (Init, IsSteamRunning, callbacks, etc.).
SOURCES = [GBE_DLL / "flat.cpp", GBE_DLL / "dll.cpp"]

# Return-type → C return statement. Anything unmatched defaults to `return 0;`
# which the C compiler casts as needed for any scalar/pointer return slot.
TYPE_RETURN = {
    "void":                  None,                         # no return
    "steam_bool":            "return 0;",                  # bool false
    "bool":                  "return 0;",
    "int":                   "return 0;",
    "int32":                 "return 0;",
    "int64":                 "return 0;",
    "uint8":                 "return 0;",
    "uint16":                "return 0;",
    "uint32":                "return 0;",
    "uint64":                "return 0;",
    "size_t":                "return 0;",
    "intptr_t":              "return 0;",
    "uint64_steamid":        "return 0;",                  # invalid SteamID
    "uint64_gameid":         "return 0;",
    "SteamAPICall_t":        "return 0;",                  # k_uAPICallInvalid
    "HSteamPipe":            "return 1;",                  # any non-zero pipe
    "HSteamUser":            "return 1;",                  # any non-zero user
    "HAuthTicket":           "return 0;",
    "EResult":               "return 1;",                  # k_EResultOK
    "EVoiceResult":          "return 0;",                  # k_EVoiceResultOK
    "EUserHasLicenseForAppResult": "return 0;",
    "ESteamAPIInitResult":   "return 0;",                  # OK
}

# const char * / char * → empty string literal
RETURNS_STRING = re.compile(r"^(const\s+)?char\s*\*\s*$")

# ISteam<X>* / pointer to any interface → return shared dummy non-NULL.
RETURNS_PTR = re.compile(r"^(ISteam\w+|void|HSteamUser|HSteamPipe)?\s*\*\s*$")

# Anything else (struct returns mostly via hidden 1st-arg pointer on Linux
# arm64 — caller passes &out, we just don't touch it; return value doesn't
# matter for >16-byte structs). For 8-or-16-byte structs returned in
# x0/x1, returning 0 fills both with zero which is normally a valid
# "zero-initialised" value of these handle-style structs.

# Specific overrides keyed by function name (applied AFTER autogen so the
# default rule produces something, then we replace).
OVERRIDES = {
    # Init / lifecycle
    "SteamInternal_SteamAPI_Init":       "return 0;",                     # OK
    "SteamAPI_InitFlat":                 "return 0;",
    "SteamAPI_Init":                     "return 1;",                     # true
    "SteamAPI_InitSafe":                 "return 1;",
    "SteamAPI_IsSteamRunning":           "return 1;",                     # game checks this
    "SteamAPI_GetHSteamUser":            "return 1;",
    "SteamAPI_GetHSteamPipe":            "return 1;",
    "SteamAPI_RestartAppIfNecessary":    "return 0;",                     # don't restart
    "SteamGameServer_Init":              "return 1;",
    "SteamInternal_GameServer_Init":     "return 0;",
    "SteamInternal_GameServer_Init_V2":  "return 0;",

    # SteamApps — language is the most common early check
    "SteamAPI_ISteamApps_GetCurrentGameLanguage":    'return "english";',
    "SteamAPI_ISteamApps_GetAvailableGameLanguages": 'return "english";',
    "SteamAPI_ISteamApps_BIsSubscribed":             "return 1;",         # own the game
    "SteamAPI_ISteamApps_BIsSubscribedApp":          "return 1;",
    "SteamAPI_ISteamApps_BIsAppInstalled":           "return 1;",
    "SteamAPI_ISteamApps_GetAppBuildId":             "return 1;",
    "SteamAPI_ISteamApps_BIsDlcInstalled":           "return 0;",

    # SteamUtils — locale & overlay
    "SteamAPI_ISteamUtils_GetIPCountry":             'return "US";',
    "SteamAPI_ISteamUtils_GetSteamUILanguage":       'return "english";',
    "SteamAPI_ISteamUtils_GetConnectedUniverse":     "return 1;",         # k_EUniversePublic
    "SteamAPI_ISteamUtils_IsOverlayEnabled":         "return 0;",
    "SteamAPI_ISteamUtils_BOverlayNeedsPresent":     "return 0;",
    "SteamAPI_ISteamUtils_IsSteamRunningInVR":       "return 0;",
    "SteamAPI_ISteamUtils_IsSteamInBigPictureMode":  "return 0;",
    "SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck":"return 0;",

    # SteamUser / SteamFriends — identity
    "SteamAPI_ISteamUser_BLoggedOn":                 "return 1;",
    "SteamAPI_ISteamFriends_GetPersonaName":         'return "Player";',

    # SteamInput / SteamController — no controllers connected
    "SteamAPI_ISteamInput_Init":                     "return 1;",
    "SteamAPI_ISteamInput_GetConnectedControllers":  "return 0;",
    "SteamAPI_ISteamController_Init":                "return 1;",
    "SteamAPI_ISteamController_GetConnectedControllers": "return 0;",
}

INTERFACE_PATTERN = re.compile(
    r"^(?:STEAMAPI_API|S_API)\s+"
    r"(?P<rtype>(?:const\s+)?[\w:]+(?:\s*\*+)?)"     # return type, optional * (with/without space)
    r"\s*(?:S_CALLTYPE)?\s*"                          # optional S_CALLTYPE, no required whitespace
    r"(?P<name>SteamAPI_\w+|SteamInternal_\w+|SteamGameServer_\w+|SteamGameServerInternal_\w+)"
    r"\s*\("
)

def classify_return(rtype: str) -> str:
    """Return the C statement for the given return type."""
    rtype = rtype.strip()
    if rtype == "void":
        return None
    if RETURNS_STRING.match(rtype):
        return 'return "";'
    # Pointer to anything — return shared dummy non-NULL.
    if rtype.endswith("*"):
        return "return _dummy_iface;"
    # Explicit table
    if rtype in TYPE_RETURN:
        return TYPE_RETURN[rtype]
    # SteamID_t / HServerListRequest / etc. — handle types fit in 8 bytes,
    # return 0 (invalid handle).
    return "return 0;"

def parse(sources):
    """Yield (name, return_statement) for every STEAMAPI_API / S_API export."""
    seen = set()
    for src in sources:
        for line in src.read_text(errors="ignore").splitlines():
            m = INTERFACE_PATTERN.match(line)
            if not m:
                continue
            name = m["name"]
            if name in seen:
                continue
            seen.add(name)
            ret = OVERRIDES.get(name, classify_return(m["rtype"]))
            yield name, ret

def main():
    funcs = list(parse(SOURCES))
    print(f"parsed {len(funcs)} unique exports from {len(SOURCES)} sources")

    lines = [
        "/* GENERATED by gen_stubs.py — do not edit by hand.",
        " * Source: gbe_fork dll/flat.cpp (Steam SDK 1.59+ flat-C surface).",
        " * Strategy: every export is a zero-arg C function returning a sane",
        " * default. arm64 cdecl puts caller args in x0-x7 and we just don't",
        " * read them, returning in x0/v0. See gen_stubs.py for type→return",
        " * mapping + manual overrides for early-call sites. */",
        "",
        "#include <stdint.h>",
        "#include <string.h>",
        "",
        "/* Shared non-NULL handle returned for every interface getter. Real",
        " * Steam returns distinct vtable pointers per interface; games check",
        " * null/non-null then make virtual calls. We can't dispatch virtual",
        " * calls (no vtable), so a single shared address is enough — the",
        " * Steamworks.NET wrappers use _flat_ C entries (this file), not",
        " * direct vtable dispatch. */",
        "static char _dummy_iface[64] = { 0 };",
        "",
    ]

    for name, ret in funcs:
        if ret is None:
            lines.append(f"void {name}(void) {{}}")
        elif ret.startswith('return "'):
            lines.append(f"const char *{name}(void) {{ {ret} }}")
        elif ret == "return _dummy_iface;":
            lines.append(f"void *{name}(void) {{ {ret} }}")
        else:
            lines.append(f"long long {name}(void) {{ {ret} }}")

    OUT_C.write_text("\n".join(lines) + "\n")
    print(f"wrote {OUT_C} ({OUT_C.stat().st_size} bytes)")

if __name__ == "__main__":
    main()
