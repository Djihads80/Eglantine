#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Scripted lab runs on top of tools/nx2/lab_ctl.py: boot a game hidden, load a save, then burst-capture,
watch for flashes or survey a walk. Runs on Linux, macOS and Windows.

    lab_scenarios.py gameplay                       boot and load the newest save (Ocarina of Time)
    lab_scenarios.py burst <tag> <frames>           gameplay, then save every presented frame for a while
    lab_scenarios.py watch <tag> <seconds>          gameplay with the in-emulator white-flash watcher
    lab_scenarios.py survey <tag> [steps]           gameplay, then a screenshot every few steps of a walk

Common options (before the scenario): --game <file> (or EDEN_NX2_GAME), -d <diag dir> (default
<temp>/ootlab), --config <qt-config.ini> (copied into the diag dir and muted; default: the user's),
--binary, --tensor, --dump-dir, --env NAME=VALUE (repeatable), --max-seconds.
PRE="<scancode> <ms>;..." in the environment turns the camera or walks before a burst or watch
(I/J/K/L = 12/13/14/15 camera).

Waits poll the screenshot's brightness and the frame counter instead of sleeping for fixed times,
so a slow first boot (pipelines compiling) or a fast machine both work. The few fixed waits are the
save-load screens' timings, set by the options below.
"""

import argparse
import glob
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True  # this folder is a skill, not a package: keep it free of __pycache__

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools", "nx2"))
import lab_ctl  # noqa: E402

BRIGHT = 0.3  # luma of a frame that is showing the game or the title
DARK = 0.12   # luma of a loading screen


def log(message):
    print(message, flush=True)


def load_helper(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# --- the emulator -------------------------------------------------------------------------------

def other_emulators():
    """Pids of eden-cli / eglantine processes that this lab did not start."""
    try:
        if lab_ctl.WINDOWS:
            out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
            rows = [line.strip('"').split('","') for line in out.splitlines() if line.startswith('"')]
            return [int(row[1]) for row in rows
                    if len(row) > 1 and re.match(r"(eden-cli|eglantine)\.exe$", row[0], re.I)]
        # Exact process names: `pgrep -f` also matches editors, tails and paths containing "eglantine".
        found = []
        for name in ("eden-cli", "eglantine"):
            out = subprocess.run(["pgrep", "-x", name], capture_output=True, text=True).stdout
            found += [int(pid) for pid in out.split()]
        return found
    except (OSError, ValueError):
        return []


def user_config():
    candidates = [os.path.join(os.environ.get("APPDATA", ""), "eglantine", "config", "qt-config.ini"),
                  os.path.expanduser("~/.config/eden/qt-config.ini"),
                  os.path.expanduser("~/.config/eglantine/qt-config.ini")]
    return next((path for path in candidates if os.path.isfile(path)), None)


BINDING = re.compile(r"^(player_0_(?:button_\w+|lstick|rstick|motion\w*))(\default)?=(.*)$", re.M)


def merge_keyboard(text, sdl_text):
    """Copies player 0's bindings from the SDL frontend's configuration into the Qt one, whose are
    usually a gamepad: key presses do nothing with those."""
    for name, default, value in BINDING.findall(sdl_text):
        line = re.compile(rf"(?m)^{re.escape(name)}{re.escape(default)}=.*$")
        text = line.sub(lambda match: f"{name}{default}={value}", text)
    return text


def prepare_config(args):
    """A scratch copy of the configuration with the keyboard bindings merged in and the audio muted:
    unattended runs are silent, keys reach the game, and the user's file stays as it is."""
    source = args.config or user_config()
    if not source:
        return None
    copy = os.path.join(args.diag_dir, "qt-copy.ini")
    shutil.copyfile(source, copy)
    sdl = args.sdl_config or os.path.join(os.path.dirname(source), "sdl2-config.ini")
    if os.path.isfile(sdl):
        with open(copy, encoding="utf-8") as handle:
            text = handle.read()
        with open(sdl, encoding="utf-8") as handle:
            merged = merge_keyboard(text, handle.read())
        with open(copy, "w", encoding="utf-8") as handle:
            handle.write(merged)
    else:
        log(f"no {sdl}: key presses only work if player 0 is bound to the keyboard in {source}")
    load_helper("mute_audio").main([copy])
    return copy


def stop(args):
    if os.path.exists(os.path.join(args.diag_dir, "emulator.pid")):
        try:
            lab_ctl.command_stop(argparse.Namespace(diag_dir=args.diag_dir))
        except SystemExit:
            pass


def launch(args, extra_env=()):
    if not args.game:
        sys.exit("no game: pass --game <file> or set EDEN_NX2_GAME")
    os.makedirs(args.diag_dir, exist_ok=True)
    stop(args)
    others = other_emulators()
    for _ in range(20):  # a stopped emulator can stay listed for a few seconds while its GPU memory is freed
        if not others:
            break
        time.sleep(0.5)
        others = other_emulators()
    if others and not args.force:
        sys.exit(f"eden-cli/eglantine is already running (pid {others}): it would share the GPU and the "
                 "log. Close it, or pass --force.")
    lab_ctl.command_launch(argparse.Namespace(
        diag_dir=args.diag_dir, game=args.game, dump_dir=args.dump_dir, tensor=args.tensor, hidden=True,
        keys=None, env=list(args.env or []) + list(extra_env), max_seconds=args.max_seconds,
        binary=args.binary, config=prepare_config(args)))


def emulator_alive(args):
    pid = lab_ctl.read(os.path.join(args.diag_dir, "emulator.pid"))
    return bool(pid) and lab_ctl.pid_alive(int(pid))


def look(args, out=None, timeout=20):
    """Settled screenshot stats, or None while the game isn't presenting."""
    try:
        return lab_ctl.capture(args.diag_dir, 1, out, timeout)[3]
    except SystemExit:
        return None


def wait_bright(args, what, timeout, quiet_after=90):
    """Polls until a frame is bright. Gives up when the emulator exits, or the frame counter has not
    moved for `quiet_after` seconds (a stall: nothing more would come)."""
    deadline = time.time() + timeout
    last_frame, last_move = None, time.time()
    while time.time() < deadline:
        if not emulator_alive(args):
            sys.exit(f"the emulator exited while waiting for {what}; see {args.diag_dir}/emulator.log")
        frame = lab_ctl.frame(args.diag_dir)
        if frame is not None and frame != last_frame:
            last_frame, last_move = frame, time.time()
        elif time.time() - last_move > quiet_after:
            sys.exit(f"no frames presented for {quiet_after} s while waiting for {what} (frame {frame})")
        result = look(args)
        if result and result["luma"] > BRIGHT:
            return result
        time.sleep(1)
    sys.exit(f"timed out after {timeout} s waiting for {what}")


def wait_dark(args, what, timeout, fatal=True):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not emulator_alive(args):
            sys.exit(f"the emulator exited while waiting for {what}; see {args.diag_dir}/emulator.log")
        result = look(args)
        if result and result["luma"] < DARK:
            return result
        time.sleep(0.5)
    if fatal:
        sys.exit(f"never saw {what} within {timeout} s: the key presses probably did not reach the game "
                 f"(check the keyboard bindings), or this save loads without a dark screen")
    return None


def key(args, code, hold_ms):
    """One key press for the running eden-cli, queued like `lab_ctl press`."""
    path = os.path.join(args.diag_dir, "keys.txt")
    deadline = time.time() + 10
    while os.path.exists(path):
        if time.time() > deadline:
            sys.exit("keys.txt is not being read: is eden-cli running with this diag dir?")
        time.sleep(0.05)
    lab_ctl.write_atomic(path, f"{code} {hold_ms}\n")
    time.sleep(hold_ms / 1000 + 0.3)


def pre_moves(args):
    for step in filter(None, os.environ.get("PRE", "").split(";")):
        code, _, ms = step.strip().partition(" ")
        key(args, int(code), int(ms))
        time.sleep(0.5)


# --- scenarios ----------------------------------------------------------------------------------

def to_gameplay(args, extra_env=()):
    """Boots, presses through the title and load screens, returns at the first bright gameplay frame."""
    launch(args, extra_env)
    a = lab_ctl.BUTTONS["a"]
    t0 = time.time()
    wait_bright(args, "the title screen", args.title_timeout)
    log(f"title up after {time.time() - t0:.0f} s")
    time.sleep(args.title_settle)
    key(args, a, 200)   # title -> load screen
    time.sleep(args.load_step)
    key(args, a, 200)   # newest save
    time.sleep(args.confirm_step)
    # Confirm. The load screen is dark and only a bright frame after it is the game; a title screen
    # is as bright as a scene. A press that arrives before the dialog is ready is lost, so press
    # again while the screen stays bright.
    for _ in range(args.confirm_tries):
        key(args, a, 200)
        if wait_dark(args, "the load screen", args.load_timeout, fatal=False):
            break
    else:
        sys.exit(f"never saw the load screen after {args.confirm_tries} confirm presses: the keys "
                 "probably do not reach the game (check the keyboard bindings), or the save did not load")
    result = wait_bright(args, "gameplay", args.gameplay_timeout)
    log(f"gameplay after {time.time() - t0:.0f} s: {json.dumps(result)}")
    return result


def scenario_gameplay(args):
    to_gameplay(args)
    log(f"running; stop with: python -I tools/nx2/lab_ctl.py -d {args.diag_dir} stop")


def scenario_burst(args):
    frames = int(args.count)
    folder = os.path.join(args.diag_dir, f"burst_{args.tag}")
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    try:
        to_gameplay(args, [f"EDEN_NX2_FRAME_CAPTURE={folder}"])
        time.sleep(5)
        pre_moves(args)
        if os.environ.get("PRE"):
            look(args, os.path.join(args.diag_dir, f"burst_{args.tag}_view.png"))
        for stale in glob.glob(os.path.join(folder, "nx2-frame-*.tga")):
            os.unlink(stale)
        marker = os.path.join(folder, "burst")
        with open(marker, "w") as handle:
            handle.write(f"{frames}\n")
        start = time.time()
        while os.path.exists(marker) and time.time() - start < 10:  # taken when the next present starts it
            time.sleep(0.5)
        # Frames arrive about every 2nd present, ~20 a second; finished when the count stops growing.
        deadline = time.time() + frames / 20 + 15
        seen, still = -1, 0
        while time.time() < deadline and still < 6:
            count = len(glob.glob(os.path.join(folder, "nx2-frame-*.tga")))
            still = still + 1 if count == seen else 0
            seen = count
            time.sleep(1)
    finally:
        keep_log(args, f"burst_{args.tag}.emulator.log")
        stop(args)
    log(f"frames: {len(glob.glob(os.path.join(folder, 'nx2-frame-*.tga')))} in {folder}")


def scenario_watch(args):
    folder = os.path.join(args.diag_dir, f"watch_{args.tag}")
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    try:
        to_gameplay(args, [f"EDEN_NX2_FRAME_CAPTURE={folder}", "EDEN_NX2_FLASH_WATCH=1"])
        since = lab_ctl.frame(args.diag_dir) or 0
        time.sleep(3)
        pre_moves(args)
        deadline = time.time() + float(args.count)
        while time.time() < deadline and emulator_alive(args):
            time.sleep(1)
    finally:
        saved = keep_log(args, f"watch_{args.tag}.emulator.log")
        stop(args)
    if saved:
        with open(saved, "rb") as handle:
            text = handle.read().replace(b"\0", b"").decode("utf-8", "replace")
        for line in [line for line in text.splitlines() if "flash watch" in line][-20:]:
            log(line)
    flashes = [int(re.findall(r"flash-(\d+)", name)[0]) for name in os.listdir(folder)
               if re.match(r"^flash-\d+\.tga$", name)]
    # Menus and fades before the game are near-white too: count what happened in the game itself.
    log(f"saved: {len(flashes)} frames in {folder}, {len([n for n in flashes if n >= since])} "
        f"from present {since} on (gameplay)")


def scenario_survey(args):
    steps = int(args.count or 8)
    folder = os.path.join(args.diag_dir, f"survey_{args.tag}")
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    forward, camera_right = lab_ctl.BUTTONS["up"], 15
    try:
        to_gameplay(args)
        for step in range(1, steps + 1):
            key(args, forward, 1400)
            time.sleep(2.2)
            if not look(args, os.path.join(folder, f"step_{step}.png")):
                log(f"capture step_{step} failed")
            key(args, camera_right, 400)
            time.sleep(1.2)
            if not look(args, os.path.join(folder, f"look_{step}.png")):
                log(f"capture look_{step} failed")
    finally:
        stop(args)
    shots = glob.glob(os.path.join(folder, "*.png"))
    if not shots:
        sys.exit("no captures: the emulator was not presenting or lab_ctl failed")
    sheet = contact_sheet(folder, shots)
    log(f"{sheet or folder}: {len(shots)} shots")


def contact_sheet(folder, shots):
    try:
        from PIL import Image
    except ImportError:
        return None
    def order(path):
        kind, _, number = os.path.basename(path)[:-4].partition("_")
        return int(number), kind
    cols = 4
    rows = (len(shots) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * 480, rows * 270))
    for index, path in enumerate(sorted(shots, key=order)):
        sheet.paste(Image.open(path).convert("RGB").resize((480, 270)),
                    ((index % cols) * 480, (index // cols) * 270))
    out = folder + ".png"
    sheet.save(out)
    return out


def keep_log(args, name):
    lab_ctl.collect_log(args.diag_dir)
    source = os.path.join(args.diag_dir, "emulator.log")
    target = os.path.join(args.diag_dir, name)
    try:
        shutil.copyfile(source, target)
        return target
    except OSError:
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0], formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--game", default=os.environ.get("EDEN_NX2_GAME"))
    parser.add_argument("-d", "--diag-dir", default=os.path.join(tempfile.gettempdir(), "ootlab"))
    parser.add_argument("--config", help="eden-cli configuration to copy (default: the user's qt-config.ini)")
    parser.add_argument("--sdl-config", help="where the keyboard bindings come from (default: sdl2-config.ini "
                                             "next to the configuration)")
    parser.add_argument("--binary")
    parser.add_argument("--dump-dir")
    parser.add_argument("--tensor", action="store_true")
    parser.add_argument("--env", action="append", help="extra NAME=VALUE for the emulator")
    parser.add_argument("--max-seconds", type=int, default=420)
    parser.add_argument("--force", action="store_true", help="launch although another eden-cli is running")
    parser.add_argument("--title-timeout", type=int, default=300, help="seconds to wait for the title")
    parser.add_argument("--title-settle", type=float, default=8, help="seconds after the first bright frame")
    parser.add_argument("--load-step", type=float, default=9, help="seconds the load screen takes to appear")
    parser.add_argument("--confirm-step", type=float, default=4, help="seconds before confirming the save")
    parser.add_argument("--load-timeout", type=int, default=10, help="seconds to wait for the load screen")
    parser.add_argument("--confirm-tries", type=int, default=5, help="confirm presses before giving up")
    parser.add_argument("--gameplay-timeout", type=int, default=240, help="seconds to wait for gameplay")
    commands = parser.add_subparsers(dest="scenario", required=True)
    commands.add_parser("gameplay").set_defaults(func=scenario_gameplay)
    for name, func, help_count in (("burst", scenario_burst, "frames"), ("watch", scenario_watch, "seconds"),
                                   ("survey", scenario_survey, "steps")):
        sub = commands.add_parser(name)
        sub.add_argument("tag")
        sub.add_argument("count", nargs="?", default="8" if name == "survey" else None, help=help_count)
        sub.set_defaults(func=func)
    args = parser.parse_args()
    args.diag_dir = os.path.abspath(os.path.expanduser(args.diag_dir))
    if args.scenario in ("burst", "watch") and not args.count:
        parser.error(f"{args.scenario} needs a count of {dict(burst='frames', watch='seconds')[args.scenario]}")
    args.func(args)


if __name__ == "__main__":
    main()
