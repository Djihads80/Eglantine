#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Command line control of the shader lab's diagnostics folder, for scripted bisection.

Works on the same files as shader_lab.py (ablate.txt, inspect.txt, draw-list, diag-state.json,
capture-<n>.tga, draw-list-<n>.tsv), so the browser lab can stay open beside it.

    lab_ctl.py -d <diag dir> launch --game <file> [--tensor] [--hidden] [--keys <script>]
    lab_ctl.py -d <diag dir> state [--kind p] [--active 120] [--limit 40]
    lab_ctl.py -d <diag dir> ablate add|remove|set|clear [key ...]
    lab_ctl.py -d <diag dir> capture [--settle 30] [--out shot.png]
    lab_ctl.py -d <diag dir> stats <image>
    lab_ctl.py -d <diag dir> drawlist [<n>]
    lab_ctl.py -d <diag dir> bisect --kind p --bad "b < 0.3 * g" [--settle 30]
    lab_ctl.py -d <diag dir> press a down*2 plus [--hold 120]
    lab_ctl.py -d <diag dir> stop

Image statistics (stats, capture, bisect) are means over the screenshot in 0..1: r, g, b, luma,
and the same over the central half (cr, cg, cb). `bisect` finds one program whose ablation turns the
--bad expression false, by halving the active programs of a kind.
"""

import argparse
import json
import os
import re
import signal
import struct
import subprocess
import sys
import threading
import time
import uuid
import zlib

KEY = re.compile(r"^[0-9a-f]{16}$")
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


class WindowsProcesses:
    """Process handles and a private Job Object; never terminate a PID by itself."""

    def __init__(self):
        import ctypes
        from ctypes import wintypes as wt
        self.ct, self.wt = ctypes, wt
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        declarations = {
            "CreateJobObjectW": ([wt.LPVOID, wt.LPCWSTR], wt.HANDLE),
            "OpenJobObjectW": ([wt.DWORD, wt.BOOL, wt.LPCWSTR], wt.HANDLE),
            "SetInformationJobObject": ([wt.HANDLE, ctypes.c_int, wt.LPVOID, wt.DWORD], wt.BOOL),
            "AssignProcessToJobObject": ([wt.HANDLE, wt.HANDLE], wt.BOOL),
            "IsProcessInJob": ([wt.HANDLE, wt.HANDLE, ctypes.POINTER(wt.BOOL)], wt.BOOL),
            "TerminateJobObject": ([wt.HANDLE, wt.UINT], wt.BOOL),
            "OpenProcess": ([wt.DWORD, wt.BOOL, wt.DWORD], wt.HANDLE),
            "CreateToolhelp32Snapshot": ([wt.DWORD, wt.DWORD], wt.HANDLE),
            "Thread32First": ([wt.HANDLE, wt.LPVOID], wt.BOOL),
            "Thread32Next": ([wt.HANDLE, wt.LPVOID], wt.BOOL),
            "OpenThread": ([wt.DWORD, wt.BOOL, wt.DWORD], wt.HANDLE),
            "GetProcessIdOfThread": ([wt.HANDLE], wt.DWORD),
            "ResumeThread": ([wt.HANDLE], wt.DWORD),
            "GetProcessTimes": ([wt.HANDLE] + [ctypes.POINTER(wt.FILETIME)] * 4, wt.BOOL),
            "QueryFullProcessImageNameW": ([wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)], wt.BOOL),
            "WaitForSingleObject": ([wt.HANDLE, wt.DWORD], wt.DWORD),
            "CloseHandle": ([wt.HANDLE], wt.BOOL),
        }
        for name, (arguments, result) in declarations.items():
            function = getattr(self.kernel, name)
            function.argtypes, function.restype = arguments, result

    def checked(self, result):
        if not result:
            raise self.ct.WinError(self.ct.get_last_error())
        return result

    def close(self, handle):
        if handle:
            self.kernel.CloseHandle(handle)

    def create_job(self, name):
        ct, wt = self.ct, self.wt
        class Limits(ct.Structure):
            _fields_ = [("process_time", ct.c_int64), ("job_time", ct.c_int64),
                        ("flags", wt.DWORD), ("min_working", ct.c_size_t),
                        ("max_working", ct.c_size_t), ("active", wt.DWORD),
                        ("affinity", ct.c_size_t), ("priority", wt.DWORD), ("scheduling", wt.DWORD)]
        class ExtendedLimits(ct.Structure):
            _fields_ = [("basic", Limits), ("io_counters", ct.c_uint64 * 6),
                        ("process_memory", ct.c_size_t), ("job_memory", ct.c_size_t),
                        ("peak_process", ct.c_size_t), ("peak_job", ct.c_size_t)]
        job = self.checked(self.kernel.CreateJobObjectW(None, name))
        limits = ExtendedLimits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        try:
            self.checked(self.kernel.SetInformationJobObject(job, 9, ct.byref(limits), ct.sizeof(limits)))
            return job
        except BaseException:
            self.close(job)
            raise

    def identity(self, process):
        times = [self.wt.FILETIME() for _ in range(4)]
        self.checked(self.kernel.GetProcessTimes(process, *(self.ct.byref(t) for t in times)))
        image = self.ct.create_unicode_buffer(32768)
        length = self.wt.DWORD(len(image))
        self.checked(self.kernel.QueryFullProcessImageNameW(process, 0, image, self.ct.byref(length)))
        return {"created": (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime,
                "image": os.path.normcase(image.value)}

    def resume(self, process):
        # Popen closes CreateProcess's primary thread handle. Find the one thread
        # of our suspended child, and resume only after the job/watchdog are ready.
        class ThreadEntry(self.ct.Structure):
            _fields_ = [("size", self.wt.DWORD), ("usage", self.wt.DWORD),
                        ("thread_id", self.wt.DWORD), ("process_id", self.wt.DWORD),
                        ("base_priority", self.wt.LONG), ("delta_priority", self.wt.LONG),
                        ("flags", self.wt.DWORD)]
        snapshot = self.kernel.CreateToolhelp32Snapshot(4, 0)  # TH32CS_SNAPTHREAD
        if snapshot == self.ct.c_void_p(-1).value:
            raise self.ct.WinError(self.ct.get_last_error())
        thread = None
        try:
            entry = ThreadEntry()
            entry.size = self.ct.sizeof(entry)
            found = self.kernel.Thread32First(snapshot, self.ct.byref(entry))
            while found:
                if entry.process_id == process.pid:
                    thread = self.checked(self.kernel.OpenThread(0x802, False, entry.thread_id))
                    break
                found = self.kernel.Thread32Next(snapshot, self.ct.byref(entry))
            if (not thread or process.poll() is not None or
                    self.kernel.GetProcessIdOfThread(thread) != process.pid):
                raise RuntimeError("suspended child thread is no longer available")
            if self.kernel.ResumeThread(thread) != 1:
                raise RuntimeError("could not resume the suspended child exactly once")
        finally:
            self.close(thread)
            self.close(snapshot)

    def open_owned(self, record):
        process = self.kernel.OpenProcess(0x101000, False, record["pid"])
        if not process:
            if self.ct.get_last_error() == 87:  # PID no longer exists.
                return None
            self.checked(process)
        try:
            if self.kernel.WaitForSingleObject(process, 0) == 0:
                self.close(process)
                return None
            identity = self.identity(process)
            if identity != {key: record[key] for key in ("created", "image")}:
                raise RuntimeError("process identity changed; refusing to stop a reused PID")
            return process
        except BaseException:
            self.close(process)
            raise

    def open_owned_job(self, record, process):
        job = self.checked(self.kernel.OpenJobObjectW(0xC, False, record["job"]))
        member = self.wt.BOOL()
        try:
            self.checked(self.kernel.IsProcessInJob(process, job, self.ct.byref(member)))
            if not member.value:
                raise RuntimeError("process is not in this lab's Job Object")
            return job
        except BaseException:
            self.close(job)
            raise

    def request_close(self, pid):
        user = self.ct.WinDLL("user32", use_last_error=True)
        callback_type = self.ct.WINFUNCTYPE(self.wt.BOOL, self.wt.HWND, self.wt.LPARAM)
        user.EnumWindows.argtypes = [callback_type, self.wt.LPARAM]
        user.GetWindowThreadProcessId.argtypes = [self.wt.HWND, self.ct.POINTER(self.wt.DWORD)]
        user.PostMessageW.argtypes = [self.wt.HWND, self.wt.UINT, self.wt.WPARAM, self.wt.LPARAM]
        requested = []
        @callback_type
        def close_window(window, unused):
            owner = self.wt.DWORD()
            user.GetWindowThreadProcessId(window, self.ct.byref(owner))
            if owner.value == pid:
                if user.PostMessageW(window, 0x10, 0, 0):  # WM_CLOSE
                    requested.append(window)
            return True
        user.EnumWindows(close_window, 0)
        return bool(requested)


def windows_record(diag):
    text = read(os.path.join(diag, "emulator.json"))
    if not text:
        return None
    record = json.loads(text)
    if (record.get("platform") != "windows" or
            not re.fullmatch(r"[0-9a-f]{32}", record.get("token", "")) or
            record.get("job") != "Local\\EglantineLab-" + record["token"] or
            not isinstance(record.get("pid"), int) or not 0 < record["pid"] < 2**32 or
            not isinstance(record.get("created"), int) or not isinstance(record.get("image"), str)):
        raise RuntimeError("invalid emulator identity file")
    return record


def windows_ready(diag, token):
    return os.path.join(diag, "watchdog-" + token + ".ready")


def command_windows_watchdog(args):
    api = WindowsProcesses()
    record = windows_record(args.diag_dir)
    if not record or record["token"] != args.token:
        sys.exit("watchdog identity mismatch")
    process = api.open_owned(record)
    if not process:
        sys.exit("emulator exited before watchdog startup")
    job = None
    try:
        job = api.open_owned_job(record, process)
        write_atomic(windows_ready(args.diag_dir, args.token), args.token)
        reason = "exited"
        while True:
            waited = api.kernel.WaitForSingleObject(process, 200)
            if waited == 0:
                break
            if waited != 258:
                raise api.ct.WinError(api.ct.get_last_error())
            if time.monotonic() >= record["deadline"]:
                api.checked(api.kernel.TerminateJobObject(job, 124))
                reason = "timeout"
                break
        write_atomic(os.path.join(args.diag_dir, "watchdog-result.json"),
                     json.dumps({"token": args.token, "reason": reason, "pid": record["pid"]}))
    finally:
        api.close(job)
        api.close(process)


def windows_available(diag):
    api = WindowsProcesses()
    previous = windows_record(diag)
    if previous:
        process = api.open_owned(previous)
        if process:
            api.close(process)
            sys.exit("this diagnostics directory already owns a running emulator; stop it first")
    elif read(os.path.join(diag, "emulator.pid")):
        sys.exit("legacy emulator.pid has no Windows ownership record; use a fresh diagnostics directory")


def windows_launch(command, environment, log, diag, max_seconds):
    windows_available(diag)
    api = WindowsProcesses()
    token = uuid.uuid4().hex
    job_name = "Local\\EglantineLab-" + token
    job = api.create_job(job_name)
    process = guard = None
    try:
        process = subprocess.Popen(command, env=environment, stdout=log, stderr=subprocess.STDOUT,
                                   creationflags=(subprocess.CREATE_NO_WINDOW |
                                                  subprocess.CREATE_NEW_PROCESS_GROUP | 4),  # CREATE_SUSPENDED
                                   cwd=REPO)
        api.checked(api.kernel.AssignProcessToJobObject(job, int(process._handle)))
        record = {"platform": "windows", "token": token, "job": job_name, "pid": process.pid,
                  "deadline": time.monotonic() + max_seconds, **api.identity(int(process._handle))}
        write_atomic(os.path.join(diag, "emulator.json"), json.dumps(record))
        write_atomic(os.path.join(diag, "emulator.pid"), str(process.pid))
        with open(os.path.join(diag, "watchdog.log"), "w", encoding="utf-8") as guard_log:
            guard = subprocess.Popen([sys.executable, "-I", os.path.abspath(__file__), "-d", diag,
                                      "_watchdog-windows", "--token", token],
                                     stdout=guard_log, stderr=subprocess.STDOUT,
                                     creationflags=subprocess.CREATE_NO_WINDOW, cwd=REPO)
        deadline = time.monotonic() + 10
        while read(windows_ready(diag, token)) != token:
            if guard.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("watchdog did not acquire the owned process; launch cancelled")
            time.sleep(0.05)
        api.resume(process)
        # Reap the helper when this controller stays alive (e.g. imported by tests).
        # In the usual CLI invocation it remains an independent hidden process.
        threading.Thread(target=guard.wait, daemon=True).start()
        return process
    except BaseException:
        api.kernel.TerminateJobObject(job, 125)
        if process and process.poll() is None:
            process.terminate()  # Popen retains the original process HANDLE.
            process.wait(timeout=10)
        if guard and guard.poll() is None:
            guard.terminate()
            guard.wait(timeout=10)
        raise
    finally:
        # The watchdog holds the remaining job handle; if it dies, its job closes.
        api.close(job)


def windows_stop(diag):
    record = windows_record(diag)
    if not record:
        sys.exit("no Windows emulator identity record")
    api = WindowsProcesses()
    process = api.open_owned(record)
    job = None
    try:
        if process:
            job = api.open_owned_job(record, process)
            graceful = api.request_close(record["pid"])
            if api.kernel.WaitForSingleObject(process, 5000 if graceful else 0) == 258:
                api.checked(api.kernel.TerminateJobObject(job, 0))
                if api.kernel.WaitForSingleObject(process, 5000) != 0:
                    raise RuntimeError("owned emulator did not terminate")
    finally:
        api.close(job)
        api.close(process)
    current = windows_record(diag)
    if current and current["token"] == record["token"]:
        for name in ("emulator.pid", "emulator.json", os.path.basename(windows_ready(diag, record["token"]))):
            try:
                os.unlink(os.path.join(diag, name))
            except FileNotFoundError:
                pass


def read(path, default=None):
    try:
        with open(path, encoding="utf-8") as file:
            return file.read()
    except OSError:
        return default


def write_atomic(path, text):
    with open(path + ".tmp", "w", encoding="utf-8") as file:
        file.write(text)
    os.replace(path + ".tmp", path)


def state(diag):
    text = read(os.path.join(diag, "diag-state.json"))
    try:
        return json.loads(text) if text else None
    except ValueError:
        return None


def frame(diag):
    current = state(diag)
    return current["frame"] if current else None


def wait_frames(diag, count, timeout):
    """Waits until the emulator has presented `count` more frames (diag-state.json, ~1 s steps)."""
    deadline = time.time() + timeout
    start = frame(diag)
    while start is None:
        if time.time() > deadline:
            sys.exit("no diag-state.json: is the emulator running with EDEN_NX2_DIAG_DIR?")
        time.sleep(0.25)
        start = frame(diag)
    while (frame(diag) or 0) < start + count:
        if time.time() > deadline:
            sys.exit(f"timed out waiting for {count} frames (at {frame(diag)}, from {start})")
        time.sleep(0.25)


# --- ablate.txt -------------------------------------------------------------------------------

def ablated(diag):
    keys = []
    for line in (read(os.path.join(diag, "ablate.txt"), "") or "").splitlines():
        words = line.split("#")[0].split()
        if len(words) == 2 and words[0] == "key" and KEY.match(words[1]):
            keys.append(words[1])
    return keys


def set_ablated(diag, keys):
    """Rewrites the active "key" lines, keeping comments and address entries."""
    path = os.path.join(diag, "ablate.txt")
    kept = [line for line in (read(path, "") or "").splitlines()
            if not (len(line.split("#")[0].split()) == 2 and line.split("#")[0].split()[0] == "key")]
    write_atomic(path, "\n".join(kept + [f"key {key}" for key in dict.fromkeys(keys)]) + "\n")


# --- images -----------------------------------------------------------------------------------

def load_rgb(path):
    """(width, height, bytes RGB) of a TGA capture (uncompressed 24/32 bit) or any PIL image."""
    with open(path, "rb") as file:
        data = file.read()
    if path.endswith(".tga"):
        id_length, kind, depth, descriptor = data[0], data[2], data[16], data[17]
        width, height = data[12] | data[13] << 8, data[14] | data[15] << 8
        if kind != 2 or depth not in (24, 32):
            sys.exit(f"{path}: unsupported TGA (type {kind}, {depth} bpp)")
        step = depth // 8
        pixels = data[18 + id_length:18 + id_length + width * height * step]
        rgb = bytearray(width * height * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = pixels[2::step], pixels[1::step], pixels[0::step]
        if not descriptor & 0x20:  # rows bottom to top
            stride = width * 3
            rgb = b"".join(rgb[row * stride:(row + 1) * stride] for row in reversed(range(height)))
        return width, height, bytes(rgb)
    from PIL import Image  # other formats only
    image = Image.open(path).convert("RGB")
    return image.width, image.height, image.tobytes()


def stats(path):
    width, height, rgb = load_rgb(path)
    total = width * height
    result = {}
    for name, offset in (("r", 0), ("g", 1), ("b", 2)):
        result[name] = round(sum(rgb[offset::3]) / (255 * total), 4)
    # Central half, where the 3D scene is and the UI mostly is not.
    rows = range(height // 4, height * 3 // 4)
    stride = width * 3
    central = b"".join(rgb[row * stride + width // 4 * 3:row * stride + width * 3 // 4 * 3] for row in rows)
    count = len(central) // 3
    for name, offset in (("cr", 0), ("cg", 1), ("cb", 2)):
        result[name] = round(sum(central[offset::3]) / (255 * max(count, 1)), 4)
    result["luma"] = round(0.2126 * result["r"] + 0.7152 * result["g"] + 0.0722 * result["b"], 4)
    result["size"] = f"{width}x{height}"
    return result


def save_png(source, target):
    width, height, rgb = load_rgb(source)
    stride = width * 3
    raw = b"".join(b"\0" + rgb[row * stride:(row + 1) * stride] for row in range(height))
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))
    with open(target, "wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                   chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def capture(diag, settle, out, timeout):
    """Waits `settle` frames, asks for a draw-list capture and returns (id, tsv, tga, stats)."""
    if settle:
        wait_frames(diag, settle, timeout)
    # Capture numbers restart with each run: new files are told apart by when they were written.
    started = time.time() - 0.5
    def fresh(name):
        try:
            return os.path.getmtime(os.path.join(diag, name)) >= started
        except OSError:
            return False
    trigger = os.path.join(diag, "draw-list")
    open(trigger, "a").close()
    deadline = time.time() + timeout
    while True:
        shots = [name for name in os.listdir(diag) if re.match(r"^capture-\d+\.tga$", name) and fresh(name)]
        if shots:
            shot = max(shots, key=lambda name: int(re.findall(r"\d+", name)[0]))
            number = int(re.findall(r"\d+", shot)[0])
            tsv = os.path.join(diag, f"draw-list-{number}.tsv")
            if fresh(os.path.basename(tsv)):
                break
        if time.time() > deadline:
            sys.exit("timed out waiting for the capture (is the game presenting frames?)")
        time.sleep(0.2)
    tga = os.path.join(diag, shot)
    # The screenshot is written by a callback: wait until its size settles.
    size = -1
    while size != os.path.getsize(tga):
        if time.time() > deadline:
            sys.exit("capture file did not finish within the timeout")
        size = os.path.getsize(tga)
        time.sleep(0.2)
    if out:
        save_png(tga, out)
    return number, tsv, tga, stats(tga)


# --- draw lists -------------------------------------------------------------------------------

def drawlist(path):
    with open(path, encoding="utf-8") as file:
        lines = file.read().splitlines()
    header = lines[0].split("\t")
    return [dict(zip(header, line.split("\t"))) for line in lines[1:]]


# --- commands ---------------------------------------------------------------------------------

def command_launch(args):
    if not 0 < args.max_seconds <= 86400:
        sys.exit("--max-seconds must be between 1 and 86400")
    os.makedirs(args.diag_dir, exist_ok=True)
    if os.name == "nt":
        windows_available(args.diag_dir)
    environment = dict(os.environ)
    environment.update({
        "EDEN_NX2_DIAG": "1",
        "EDEN_NX2_DIAG_DIR": args.diag_dir,
        "EDEN_NX2_DIAG_SAMPLES": "1",
    })
    if args.dump_dir:
        environment["EDEN_NX2_SHADER_DUMP"] = args.dump_dir
    if args.tensor:
        environment["EDEN_NX2_TENSOR"] = "1"
    if args.hidden:
        environment["EDEN_NX2_HIDE_WINDOW"] = "1"
    if args.keys:
        environment["EDEN_NX2_KEY_SCRIPT"] = args.keys
    for item in args.env or []:
        name, _, value = item.partition("=")
        environment[name] = value
    for stale in os.listdir(args.diag_dir):
        if re.match(r"^(diag-state\.json|draw-list|draw-list-\d+\.tsv|capture-\d+\.tga)$", stale):
            os.unlink(os.path.join(args.diag_dir, stale))
    binary = args.binary or os.path.join(REPO, "build", "bin", "eden-cli.exe" if os.name == "nt" else "eden-cli")
    command = [os.path.abspath(binary)]
    if args.config:
        command += ["-c", os.path.abspath(args.config)]
    command += ["-g", os.path.abspath(args.game)]
    log_path = os.path.join(args.diag_dir, "emulator.log")
    with open(log_path, "w", encoding="utf-8") as log:
        if os.name == "nt":
            process = windows_launch(command, environment, log, args.diag_dir, args.max_seconds)
        else:
            process = subprocess.Popen(command, env=environment, stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=True, cwd=REPO)
            write_atomic(os.path.join(args.diag_dir, "emulator.pid"), str(process.pid))
    if os.name != "nt":
        # A watchdog, so a forgotten run does not keep the GPU busy: kills the process group after
        # that long unless the emulator is already gone.
        subprocess.Popen(["/bin/sh", "-c", f"sleep {args.max_seconds}; kill -TERM -{process.pid} 2>/dev/null; "
                          f"sleep 10; kill -KILL -{process.pid} 2>/dev/null"], start_new_session=True,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(json.dumps({"pid": process.pid, "log": log_path, "max_seconds": args.max_seconds}))


def command_stop(args):
    if os.name == "nt":
        windows_stop(args.diag_dir)
        print("stopped")
        return
    pid = read(os.path.join(args.diag_dir, "emulator.pid"))
    if not pid:
        sys.exit("no emulator.pid")
    try:
        os.killpg(int(pid), signal.SIGTERM)
        for _ in range(50):
            os.kill(int(pid), 0)
            time.sleep(0.2)
        os.killpg(int(pid), signal.SIGKILL)
    except ProcessLookupError:
        pass
    os.unlink(os.path.join(args.diag_dir, "emulator.pid"))
    print("stopped")


def active_programs(diag, kind, recent):
    current = state(diag) or {"frame": 0, "programs": []}
    return [program for program in current["programs"]
            if (not kind or program["kind"] == kind) and program["key"] != "0" * 16
            and current["frame"] - program["last_frame"] <= recent]


def command_state(args):
    current = state(args.diag_dir)
    if not current:
        sys.exit("no diag-state.json yet")
    programs = active_programs(args.diag_dir, args.kind, args.active)
    programs.sort(key=lambda program: -program["per_s"])
    print(f"frame {current['frame']}, {len(programs)} programs, ablated {len(ablated(args.diag_dir))}")
    for program in programs[:args.limit]:
        probe = program.get("probe")
        print(f"{program['kind']} {program['key']} {program['per_s']:>6}/s rt {program['rt']:>10} "
              f"{json.dumps(program['outcomes'])}" + (f" probe {probe}" if probe else ""))


def command_ablate(args):
    keys = args.keys
    if any(not KEY.match(key) for key in keys):
        sys.exit("keys are 16 hex digits")
    current = ablated(args.diag_dir)
    if args.action == "add":
        current += keys
    elif args.action == "remove":
        current = [key for key in current if key not in keys]
    elif args.action == "set":
        current = keys
    elif args.action == "clear":
        current = []
    set_ablated(args.diag_dir, current)
    print(f"ablated {len(set(current))}")


def command_capture(args):
    number, tsv, tga, result = capture(args.diag_dir, args.settle, args.out, args.timeout)
    result.update({"capture": number, "tga": tga, "draw_list": tsv})
    if args.out:
        result["png"] = os.path.abspath(args.out)
    print(json.dumps(result))


# Switch buttons as the SDL frontend maps them (sdl2-config.ini, player 0 defaults), and the left
# stick on the arrow keys.
BUTTONS = {"a": 4, "b": 22, "x": 29, "y": 27, "l": 9, "r": 11, "zl": 20, "zr": 26, "plus": 16,
           "minus": 17, "dleft": 30, "dup": 31, "dright": 5, "up": 82, "down": 81, "left": 80,
           "right": 79}


def command_press(args):
    """Queues key presses for the running eden-cli (<diag dir>/keys.txt, read live)."""
    lines = []
    for item in args.keys:
        name, _, count = item.partition("*")
        code = BUTTONS.get(name.lower()) or (int(name) if name.isdigit() else None)
        if code is None:
            sys.exit(f"unknown key {name} (one of {', '.join(BUTTONS)} or a scancode)")
        for _ in range(int(count or 1)):
            lines.append(f"{code} {args.hold}")
    path = os.path.join(args.diag_dir, "keys.txt")
    # Taps of one key in a row need a gap: each is sent as its own file once the last is taken.
    for line in lines:
        deadline = time.time() + 10
        while os.path.exists(path):
            if time.time() > deadline:
                sys.exit("keys.txt is not being read: is eden-cli running with this diag dir?")
            time.sleep(0.05)
        write_atomic(path, line + "\n")
        time.sleep(args.hold / 1000 + args.gap)
    print(f"pressed {len(lines)}")


def command_stats(args):
    print(json.dumps(stats(args.image)))


def command_drawlist(args):
    if args.number is None:
        numbers = [int(match.group(1)) for name in os.listdir(args.diag_dir)
                   if (match := re.match(r"^draw-list-(\d+)\.tsv$", name))]
        if not numbers:
            sys.exit("no draw lists")
        args.number = max(numbers)
    rows = drawlist(os.path.join(args.diag_dir, f"draw-list-{args.number}.tsv"))
    # Consecutive rows with the same programs and target are folded into one line.
    previous, run = None, 0
    def flush():
        if previous:
            row = previous
            print(f"{row['seq']:>5} x{run:<4} {row['kind']:<8} vs {row.get('vs_key', '-'):<16} "
                  f"ps {row.get('ps_key', '-'):<16} {row['outcome']:<8} rt {row['rt0']} "
                  f"{row['width']}x{row['height']} fmt {row['format']} samples {row.get('samples', '-')}"
                  + (f" {row['extra']}" if row.get("extra") else ""))
    for row in rows:
        signature = (row["kind"], row.get("vs_key"), row.get("ps_key"), row["outcome"], row["rt0"])
        if previous and signature == (previous["kind"], previous.get("vs_key"), previous.get("ps_key"),
                                      previous["outcome"], previous["rt0"]):
            run += 1
            continue
        flush()
        previous, run = row, 1
    flush()


def command_bisect(args):
    diag = args.diag_dir
    base = ablated(diag)
    expression = compile(args.bad, "--bad", "eval")
    def bad(extra):
        set_ablated(diag, base + extra)
        _, _, _, result = capture(diag, args.settle, None, args.timeout)
        verdict = bool(eval(expression, {"__builtins__": {}}, dict(result)))
        print(f"  ablating {len(extra):>4}: {json.dumps(result)} -> {'bad' if verdict else 'good'}",
              flush=True)
        return verdict
    candidates = sorted({program["key"] for program in active_programs(diag, args.kind, args.active)}
                        - set(base))
    print(f"{len(candidates)} active {args.kind or 'any'} programs", flush=True)
    try:
        if not bad([]):
            sys.exit("the image is already good with nothing extra ablated")
        if bad(candidates):
            sys.exit("still bad with every candidate ablated: the cause is elsewhere")
        while len(candidates) > 1:
            half = candidates[:len(candidates) // 2]
            candidates = half if not bad(half) else candidates[len(candidates) // 2:]
        print(f"culprit {args.kind or ''} {candidates[0]}")
    finally:
        set_ablated(diag, base)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("-d", "--diag-dir", required=True)
    commands = parser.add_subparsers(dest="command", required=True)

    launch = commands.add_parser("launch", help="start eden-cli with the diagnostics enabled")
    launch.add_argument("--game", required=True)
    launch.add_argument("--dump-dir")
    launch.add_argument("--tensor", action="store_true", help="set EDEN_NX2_TENSOR=1")
    launch.add_argument("--hidden", action="store_true", help="set EDEN_NX2_HIDE_WINDOW=1")
    launch.add_argument("--keys", help="EDEN_NX2_KEY_SCRIPT (\"seconds:code,...\" or \"@file\")")
    launch.add_argument("--env", action="append", help="extra NAME=VALUE")
    launch.add_argument("--max-seconds", type=int, default=900,
                        help="stop the emulator after this many seconds (1-86400)")
    launch.add_argument("--binary", "--exe", dest="binary")
    launch.add_argument("--config", help="eden-cli -c: a configuration file (e.g. a copy of qt-config.ini)")
    launch.set_defaults(func=command_launch)

    commands.add_parser("stop").set_defaults(func=command_stop)
    watchdog = commands.add_parser("_watchdog-windows", help=argparse.SUPPRESS)
    watchdog.add_argument("--token", required=True)
    watchdog.set_defaults(func=command_windows_watchdog)

    show = commands.add_parser("state", help="programs seen recently, busiest first")
    show.add_argument("--kind", choices=("v", "p", "c"))
    show.add_argument("--active", type=int, default=120, help="seen within this many frames")
    show.add_argument("--limit", type=int, default=60)
    show.set_defaults(func=command_state)

    ablate = commands.add_parser("ablate")
    ablate.add_argument("action", choices=("add", "remove", "set", "clear"))
    ablate.add_argument("keys", nargs="*")
    ablate.set_defaults(func=command_ablate)

    for name, func in (("capture", command_capture), ("bisect", command_bisect)):
        sub = commands.add_parser(name)
        sub.add_argument("--settle", type=int, default=30, help="frames to wait first")
        sub.add_argument("--timeout", type=float, default=120)
        sub.set_defaults(func=func)
        if name == "capture":
            sub.add_argument("--out", help="also save the screenshot as PNG")
        else:
            sub.add_argument("--kind", choices=("v", "p", "c"))
            sub.add_argument("--active", type=int, default=120)
            sub.add_argument("--bad", required=True, help="Python expression over the image stats")

    press = commands.add_parser("press", help="tap buttons on the running eden-cli: a b plus up*3 ...")
    press.add_argument("keys", nargs="+", help="button names (or scancodes), name*N to repeat")
    press.add_argument("--hold", type=int, default=120, help="milliseconds each press is held")
    press.add_argument("--gap", type=float, default=0.3, help="seconds between presses")
    press.set_defaults(func=command_press)

    image = commands.add_parser("stats")
    image.add_argument("image")
    image.set_defaults(func=command_stats)

    listing = commands.add_parser("drawlist", help="the newest (or <n>th) draw list, folded")
    listing.add_argument("number", nargs="?", type=int)
    listing.set_defaults(func=command_drawlist)

    args = parser.parse_args()
    args.diag_dir = os.path.abspath(os.path.expanduser(args.diag_dir))
    args.func(args)


if __name__ == "__main__":
    main()
