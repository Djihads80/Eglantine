#!/usr/bin/env python3
"""Makes the lab's emulator config: your Qt settings with player 1 on the keyboard.

The lab drives the game with key presses, but the Qt app's player 1 is usually a gamepad. This
copies your qt-config.ini and replaces player 1's buttons, sticks and motion with the keyboard
bindings from sdl2-config.ini (eden-cli's own config, which defaults to the keyboard). Graphics
settings stay yours, so lab numbers match what you see when you play.

    python -I tools/nx2/lab/make_lab_config.py [--qt <qt-config.ini>] [--sdl <sdl2-config.ini>]
                                               [--out tools/nx2/lab/lab-config.ini]

The output holds your local paths, so it is git-ignored. Run it again after changing settings.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONFIG_DIR = Path(os.environ.get("APPDATA", Path.home() / ".config")) / "eglantine" / "config"
INPUT_KEY = re.compile(r"^(player_0_(?:button_[a-z0-9_]+|lstick|rstick|motion[a-z0-9_]*))(\\default)?=")


def input_lines(path: Path) -> dict[str, str]:
    lines: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = INPUT_KEY.match(line)
        if match and not match.group(2) and "color" not in match.group(1):
            lines[match.group(1)] = line
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--qt", type=Path, default=CONFIG_DIR / "qt-config.ini")
    parser.add_argument("--sdl", type=Path, default=CONFIG_DIR / "sdl2-config.ini")
    parser.add_argument("--out", type=Path, default=HERE / "lab-config.ini")
    args = parser.parse_args()
    for path in (args.qt, args.sdl):
        if not path.is_file():
            print(f"[make_lab_config] not found: {path}. Run the emulator (Qt app and eden-cli) once first,"
                  " or pass --qt/--sdl.")
            return 1
    keyboard = input_lines(args.sdl)
    if not keyboard:
        print(f"[make_lab_config] no player 1 bindings in {args.sdl}")
        return 1
    out: list[str] = []
    replaced = set()
    for line in args.qt.read_text(encoding="utf-8", errors="replace").splitlines():
        match = INPUT_KEY.match(line)
        if match and match.group(1) in keyboard:
            key = match.group(1)
            if match.group(2):
                out.append(f"{key}\\default=false")
            else:
                out.append(keyboard[key])
                replaced.add(key)
            continue
        out.append(line)
    args.out.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"[make_lab_config] wrote {args.out}: {len(replaced)} player 1 bindings now on the keyboard")
    return 0


if __name__ == "__main__":
    sys.exit(main())
