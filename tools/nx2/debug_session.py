#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Start Eglantine for an unattended debugging session: the MCP server and the NX2 debug server are
on, and tools/nx2/session_watch.py records evidence next to it, including after a crash.

    python3 -I tools/nx2/debug_session.py [--binary EXE] [--watch-args "..."] [--] [emulator args...]

Evidence goes to <data>/debug-sessions/<timestamp>/ (see POSTMORTEM.md there). EGLANTINE_BIN
overrides the binary and WATCH_ARGS the watcher options (default: IPC-trace nx2:). Works on Linux
and Windows:

- Linux: kernel core dumps are off for the run (tens of GB); tools/nx2/crashtrace.c is built on
  first use and preloaded to record a backtrace of the faulting thread.
- Windows: EDEN_NX2_CRASH_DUMP makes eden-cli write a minidump (the Qt build writes one itself),
  which the watcher reads with cdb; the stack eden-cli prints to stderr is kept in stderr.txt.
"""

import argparse
import os
import shlex
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE))
import session_watch  # noqa: E402

WINDOWS = os.name == "nt"


def tee(stream, path):
    """Copies the emulator's stderr to the terminal and to a file as it arrives."""
    with open(path, "wb") as out:
        for chunk in iter(lambda: stream.readline(), b""):
            out.write(chunk)
            out.flush()
            sys.stderr.buffer.write(chunk)
            sys.stderr.buffer.flush()


def build_crashtrace(root):
    """crashtrace.so (Linux): rebuilt when its source is newer. None if there is no compiler."""
    preload = root / "crashtrace.so"
    source = HERE / "crashtrace.c"
    if preload.is_file() and preload.stat().st_mtime >= source.stat().st_mtime:
        return preload
    compiler = shutil.which("gcc") or shutil.which("cc")
    if compiler and subprocess.run([compiler, "-shared", "-fPIC", "-O1", "-o", str(preload), str(source)]).returncode == 0:
        return preload
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--binary", default=os.environ.get("EGLANTINE_BIN"),
                        help="the emulator (default: build/bin/eglantine)")
    parser.add_argument("--watch-args", default=os.environ.get("WATCH_ARGS"),
                        help="session_watch.py options (default: --ipc-prefix nx2:; empty: none)")
    parser.add_argument("emulator_args", nargs="*", help="passed to the emulator")
    args = parser.parse_args()

    exe = Path(args.binary) if args.binary else REPO / "build" / "bin" / ("eglantine.exe" if WINDOWS else "eglantine")
    if not exe.is_file():
        sys.exit(f"no binary at {exe}")
    data = session_watch.data_dir(str(exe))
    root = data / "debug-sessions"
    session = root / time.strftime("%Y%m%d-%H%M%S")
    session.mkdir(parents=True)

    # The emulator rotates its log on launch: keep the previous run's.
    for name in ("eden_log.txt", "eden_log.txt.old.txt"):
        source = data / "log" / name
        if source.is_file():
            shutil.copy2(source, session / f"prev-{name}")

    watch_args = "--ipc-prefix nx2:" if args.watch_args is None else args.watch_args
    watcher_log = open(session / "watch.log", "wb")
    watcher = subprocess.Popen([sys.executable, "-I", str(HERE / "session_watch.py"), "--out", str(session),
                                "--binary", str(exe), "--symbols", str(root / "symbols"),
                                *shlex.split(watch_args)], stdout=watcher_log, stderr=subprocess.STDOUT)
    print(f"session  {session}")
    print(f"watcher  tail -f {session / 'watch.log'}")

    env = dict(os.environ)
    env.setdefault("EDEN_NX2_DEBUG_SERVER", "127.0.0.1:6545")
    popen_extra = {}
    if WINDOWS:
        dumps = data / "crash_dumps"
        dumps.mkdir(parents=True, exist_ok=True)
        env.setdefault("EDEN_NX2_CRASH_DUMP", str(dumps))
    else:
        import resource
        popen_extra["preexec_fn"] = lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        preload = build_crashtrace(root)
        if preload:
            env["LD_PRELOAD"] = str(preload) + (":" + env["LD_PRELOAD"] if env.get("LD_PRELOAD") else "")
            env["CRASHTRACE_FILE"] = str(session / "crashtrace.txt")

    emulator = subprocess.Popen([str(exe), "--mcp", *args.emulator_args], env=env,
                                stderr=subprocess.PIPE, **popen_extra)
    copier = threading.Thread(target=tee, args=(emulator.stderr, session / "stderr.txt"), daemon=True)
    copier.start()
    try:
        status = emulator.wait()
    except KeyboardInterrupt:
        emulator.terminate()
        status = emulator.wait()
    copier.join(5)
    message = f"emulator exited with status {status}"
    print(message)
    (session / "exit.txt").write_text(message + "\n")

    # The watcher notices the server is gone after a few seconds, then collects the evidence.
    report = session / "POSTMORTEM.md"
    for _ in range(40):
        if report.is_file():
            break
        time.sleep(1)
    watcher.terminate()
    watcher.wait()
    watcher_log.close()

    if report.is_file():
        print(f"post-mortem  {report}")
        shown, printing = 0, False
        for line in report.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("## "):
                printing = line.startswith("## Minidump")
            if printing and shown < 40:
                print(line)
                shown += 1
    return status


if __name__ == "__main__":
    sys.exit(main())
