#!/usr/bin/env python3
"""Silence the audio of an unattended eden-cli run by editing its configuration file.

Usage: mute_audio.py <config.ini> [<config.ini> ...]

Sets output_engine=3 (AudioEngine::Null), audio_muted=true and volume=0 in the [Audio] section.
The "<key>\\default=false" line matters: with "\\default=true" the value is ignored and the
frontend falls back to the default (SDL3 sink), which is why muting through the value alone
did nothing.
"""
import re
import sys

WANTED = {"output_engine": "3", "audio_muted": "true", "volume": "0"}


def mute(text):
    for key, value in WANTED.items():
        # A lambda replacement, so the backslash in "<key>\default" is not read as an escape.
        text = re.sub(rf"(?m)^{key}\\default=\S*", lambda match, k=key: k + "\\default=false", text)
        text = re.sub(rf"(?m)^{key}=.*", f"{key}={value}", text)
    return text


def main(paths):
    for path in paths:
        try:
            with open(path, encoding="utf-8") as handle:
                text = handle.read()
        except OSError as error:
            print(f"mute_audio: {path}: {error}", file=sys.stderr)
            continue
        muted = mute(text)
        if muted != text:
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(muted)
            print(f"mute_audio: silenced {path}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
