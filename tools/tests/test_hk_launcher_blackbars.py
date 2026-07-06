#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / "pocs" / "hk-launcher" / "launcher.sh"


def main() -> int:
    src = LAUNCHER.read_text()
    assert "DisableBlackBars" in src
    assert "DisableBlackBars:1" in src
    assert "hk_sync_graphics_resolution" in src or "hk_sync_graphics_settings" in src
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
