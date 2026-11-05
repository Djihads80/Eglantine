#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Controller checks use short synthetic Python children, never an emulator/game."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location("lab_ctl", Path(__file__).with_name("lab_ctl.py"))
lab = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lab)


class ControllerFilesTests(unittest.TestCase):
    def test_unbounded_launch_rejected_before_touching_files(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(SystemExit):
                lab.command_launch(SimpleNamespace(diag_dir=folder, max_seconds=0))
            self.assertEqual(os.listdir(folder), [])

    def test_press_file_protocol_and_repeated_keys(self):
        with tempfile.TemporaryDirectory() as folder:
            received = []
            def consume():
                deadline = time.monotonic() + 3
                path = Path(folder, "keys.txt")
                while len(received) < 2 and time.monotonic() < deadline:
                    if path.exists():
                        received.append(path.read_text())
                        path.unlink()
                    time.sleep(0.01)
            reader = threading.Thread(target=consume)
            reader.start()
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    lab.command_press(SimpleNamespace(diag_dir=folder, keys=["a*2"], hold=20, gap=0.05))
            finally:
                reader.join(timeout=4)
            self.assertEqual(received, ["4 20\n", "4 20\n"])

    def test_capture_converts_native_file_protocol_without_pillow(self):
        with tempfile.TemporaryDirectory() as folder:
            def present():
                deadline = time.monotonic() + 3
                while not Path(folder, "draw-list").exists():
                    if time.monotonic() > deadline:
                        return
                    time.sleep(0.01)
                header = bytearray(18)
                header[2], header[12], header[14], header[16], header[17] = 2, 2, 2, 24, 0x20
                Path(folder, "capture-1.tga").write_bytes(header + bytes([0, 0, 255]) * 4)
                Path(folder, "draw-list-1.tsv").write_text("seq\tkind\n1\tgraphics\n")
            producer = threading.Thread(target=present)
            producer.start()
            out = str(Path(folder, "capture.png"))
            result = lab.capture(folder, 0, out, 3)
            producer.join(timeout=4)
            self.assertEqual(result[0], 1)
            self.assertEqual(result[3]["r"], 1)
            self.assertEqual(Path(out).read_bytes()[:8], b"\x89PNG\r\n\x1a\n")

    def test_posix_launch_keeps_process_group_and_config_arguments(self):
        with tempfile.TemporaryDirectory() as folder:
            args = SimpleNamespace(diag_dir=folder, max_seconds=5, dump_dir=None, tensor=False,
                                   hidden=True, keys=None, env=None, binary="/tmp/eden-cli",
                                   config="/tmp/settings.ini", game="/tmp/test.nup")
            with mock.patch.object(lab.os, "name", "posix"), mock.patch.object(lab.subprocess, "Popen") as popen:
                popen.return_value.pid = 12345
                with contextlib.redirect_stdout(io.StringIO()):
                    lab.command_launch(args)
            self.assertTrue(popen.call_args_list[0].kwargs["start_new_session"])
            self.assertEqual(popen.call_args_list[0].args[0][-4:],
                             ["-c", os.path.abspath(args.config), "-g", os.path.abspath(args.game)])
            self.assertEqual(popen.call_args_list[1].args[0][0], "/bin/sh")


@unittest.skipUnless(os.name == "nt", "Windows process ownership checks")
class WindowsOwnershipTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="eglantine-lab-test-")
        self.folder = self.temp.name
        self.process = None

    def tearDown(self):
        try:
            if self.process:
                lab.windows_stop(self.folder)
        finally:
            if self.process and self.process.poll() is None:
                self.process.terminate()
                self.process.wait(timeout=5)
            # The hidden watchdog may be finishing its final event file.
            time.sleep(0.3)
            self.temp.cleanup()

    def launch(self, seconds=10):
        with open(Path(self.folder, "child.log"), "w") as log:
            self.process = lab.windows_launch([sys.executable, "-I", "-c", "import time; time.sleep(60)"],
                                              dict(os.environ), log, self.folder, seconds)
        return lab.windows_record(self.folder)

    def test_watchdog_terminates_only_its_owned_process(self):
        unrelated = subprocess.Popen([sys.executable, "-I", "-c", "import time; time.sleep(60)"],
                                     creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            record = self.launch(1)
            self.process.wait(timeout=5)
            self.assertEqual(self.process.returncode, 124)
            self.assertIsNone(unrelated.poll())
            self.assertEqual(record["platform"], "windows")
            self.assertGreater(record["created"], 0)
        finally:
            unrelated.terminate()
            unrelated.wait(timeout=5)

    def test_watchdog_also_contains_a_child_spawned_immediately(self):
        pid_file = Path(self.folder, "grandchild.pid")
        code = ("import pathlib, subprocess, sys, time; "
                "child = subprocess.Popen([sys.executable, '-I', '-c', 'import time; time.sleep(60)']); "
                f"pathlib.Path({str(pid_file)!r}).write_text(str(child.pid)); time.sleep(60)")
        with open(Path(self.folder, "child.log"), "w") as log:
            self.process = lab.windows_launch([sys.executable, "-I", "-c", code],
                                              dict(os.environ), log, self.folder, 2)
        deadline = time.monotonic() + 1.5
        while not pid_file.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(pid_file.exists(), "synthetic child did not start")
        api = lab.WindowsProcesses()
        child = api.checked(api.kernel.OpenProcess(0x101000, False, int(pid_file.read_text())))
        try:
            self.process.wait(timeout=5)
            self.assertEqual(self.process.returncode, 124)
            self.assertEqual(api.kernel.WaitForSingleObject(child, 1000), 0)
        finally:
            api.close(child)

    def test_watchdog_death_closes_job_and_terminates_child(self):
        children = []
        popen = subprocess.Popen
        def track(*args, **kwargs):
            child = popen(*args, **kwargs)
            children.append(child)
            return child
        with mock.patch.object(lab.subprocess, "Popen", side_effect=track):
            self.launch()
        guard = children[1]
        guard.terminate()
        guard.wait(timeout=5)
        self.process.wait(timeout=5)
        self.assertIsNotNone(self.process.returncode)

    def test_stop_uses_job_and_removes_ownership_files(self):
        self.launch()
        lab.windows_stop(self.folder)
        self.process.wait(timeout=5)
        self.assertFalse(Path(self.folder, "emulator.json").exists())
        self.process = None

    def test_creation_time_mismatch_refuses_to_kill(self):
        record = self.launch()
        altered = dict(record, created=record["created"] + 1)
        Path(self.folder, "emulator.json").write_text(json.dumps(altered))
        try:
            with self.assertRaisesRegex(RuntimeError, "identity changed"):
                lab.windows_stop(self.folder)
            self.assertIsNone(self.process.poll())
        finally:
            Path(self.folder, "emulator.json").write_text(json.dumps(record))

    def test_duplicate_launch_refused_before_truncating_live_log(self):
        self.launch()
        marker = Path(self.folder, "emulator.log")
        marker.write_text("keep this log")
        with self.assertRaises(SystemExit):
            lab.command_launch(SimpleNamespace(diag_dir=self.folder, max_seconds=3))
        self.assertEqual(marker.read_text(), "keep this log")
        self.assertIsNone(self.process.poll())


if __name__ == "__main__":
    unittest.main()
