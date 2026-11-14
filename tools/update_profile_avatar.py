#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerate the built-in user avatar JPEG from dist/eden_profile.jpg.

The default user avatar is compiled in, not loaded from disk: the JPEG bytes live in
Core::Constants::ACCOUNT_BACKUP_JPEG in src/core/constants.cpp, with the length in
src/core/constants.h. Editing dist/eden_profile.jpg and rebuilding does nothing until this
script has pasted the new bytes into the array.

    python3 -I tools/update_profile_avatar.py           # rewrite the array from the jpg
    python3 -I tools/update_profile_avatar.py --check    # exit 1 if the array is out of sync

Constraints the image must satisfy:

  * valid JPEG - the array is handed to the Qt frontend and to the guest as-is.
  * at most 0x20000 bytes - acc.cpp's LoadImage fallback writes the array to the guest without
    running it through SanitizeJPEGImageSize, so an oversized avatar is rejected in-game.
  * 256x256 - the size the guest profile expects; other sizes are rescaled by the guest path
    but shown untransformed by the frontend.
"""

import argparse
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
JPEG_PATH = REPO_ROOT / "dist" / "eden_profile.jpg"
CPP_PATH = REPO_ROOT / "src" / "core" / "constants.cpp"
HEADER_PATH = REPO_ROOT / "src" / "core" / "constants.h"

MAX_JPEG_SIZE = 0x20000
PROFILE_DIMENSIONS = 256
BYTES_PER_LINE = 16

CPP_ARRAY_RE = re.compile(
    r"const std::array<u8, \d+> ACCOUNT_BACKUP_JPEG\{.*?\n        \}\};", re.DOTALL
)
HEADER_SIZE_RE = re.compile(r"extern const std::array<u8, \d+> ACCOUNT_BACKUP_JPEG;")
JPEG_SOI = b"\xff\xd8"
JPEG_SOF_MARKERS = {0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF}


def jpeg_dimensions(data: bytes) -> tuple[int, int]:
    """Read the frame size out of the JPEG SOF marker."""
    offset = 2
    while offset + 3 < len(data):
        if data[offset] != 0xFF:
            offset += 1
            continue
        marker = data[offset + 1]
        if marker in JPEG_SOF_MARKERS:
            height = int.from_bytes(data[offset + 5 : offset + 7], "big")
            width = int.from_bytes(data[offset + 7 : offset + 9], "big")
            return width, height
        if marker in (0xD8, 0x01) or 0xD0 <= marker <= 0xD7:
            offset += 2
            continue
        offset += 2 + int.from_bytes(data[offset + 2 : offset + 4], "big")
    raise ValueError("no SOF marker found")


def array_literal(data: bytes) -> str:
    body = "\n".join(
        "        " + ", ".join(f"0x{byte:02x}" for byte in data[i : i + BYTES_PER_LINE]) + ","
        for i in range(0, len(data), BYTES_PER_LINE)
    )
    # Not an f-string on purpose: the closing braces are literal text and must stay doubled.
    closing = "        }};"
    return (
        f"const std::array<u8, {len(data)}> ACCOUNT_BACKUP_JPEG{{\n"
        f"        {{\n{body}\n{closing}"
    )


def embedded_bytes() -> bytes:
    """The bytes currently pasted into constants.cpp."""
    text = CPP_PATH.read_text()
    match = CPP_ARRAY_RE.search(text)
    if match is None:
        sys.exit(f"{CPP_PATH}: ACCOUNT_BACKUP_JPEG array not found")
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", match.group(0)))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="report whether the array matches the jpg instead of rewriting it",
    )
    args = parser.parse_args()

    data = JPEG_PATH.read_bytes()
    if not data.startswith(JPEG_SOI):
        sys.exit(f"{JPEG_PATH} is not a JPEG")
    if len(data) > MAX_JPEG_SIZE:
        sys.exit(
            f"{JPEG_PATH} is {len(data)} bytes, over the {MAX_JPEG_SIZE} byte limit the guest "
            f"accepts for a user avatar; re-encode it smaller"
        )
    width, height = jpeg_dimensions(data)
    if (width, height) != (PROFILE_DIMENSIONS, PROFILE_DIMENSIONS):
        print(
            f"warning: {JPEG_PATH.name} is {width}x{height}, expected "
            f"{PROFILE_DIMENSIONS}x{PROFILE_DIMENSIONS}",
            file=sys.stderr,
        )

    current = embedded_bytes()
    if args.check:
        if current == data:
            print(f"{CPP_PATH.name} matches {JPEG_PATH.name} ({len(data)} bytes)")
            return 0
        print(
            f"{CPP_PATH.name} is out of sync with {JPEG_PATH.name}: embedded {len(current)} "
            f"bytes, jpg {len(data)} bytes. Run tools/update_profile_avatar.py",
            file=sys.stderr,
        )
        return 1

    cpp_text = CPP_PATH.read_text()
    if CPP_ARRAY_RE.search(cpp_text) is None:
        sys.exit(f"{CPP_PATH}: ACCOUNT_BACKUP_JPEG array not found")
    new_cpp_text = CPP_ARRAY_RE.sub(lambda _: array_literal(data), cpp_text, count=1)
    if CPP_ARRAY_RE.search(new_cpp_text) is None:
        sys.exit("generated array does not parse back; refusing to write it")
    CPP_PATH.write_text(new_cpp_text)

    header_text = HEADER_PATH.read_text()
    if HEADER_SIZE_RE.search(header_text) is None:
        sys.exit(f"{HEADER_PATH}: ACCOUNT_BACKUP_JPEG declaration not found")
    HEADER_PATH.write_text(
        HEADER_SIZE_RE.sub(
            f"extern const std::array<u8, {len(data)}> ACCOUNT_BACKUP_JPEG;", header_text, count=1
        )
    )

    if embedded_bytes() != data:
        sys.exit("round-trip check failed: constants.cpp does not hold the jpg bytes")
    print(f"pasted {len(data)} bytes of {JPEG_PATH.name} into {CPP_PATH.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
