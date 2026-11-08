#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

import base64
import io
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
# -I leaves the script's directory off sys.path.
sys.path.insert(0, HERE)

import debug_mcp  # noqa: E402

PNG_BYTES = b"\x89PNG fake"
PNG = base64.b64encode(PNG_BYTES).decode()


class FakeEmulator:
    """The debug server's wire protocol: one JSON request line, one JSON response line."""

    def __init__(self, capture_dir):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.port = self.listener.getsockname()[1]
        self.capture_dir = capture_dir
        self.requests = []
        threading.Thread(target=self.serve, daemon=True).start()

    def answer(self, request):
        method = request["method"]
        if method == "status":
            return {"result": {"title": "Test", "presents": 3}}
        if method == "screenshot":
            return {"result": {"width": 1280, "height": 720, "png_base64": PNG}}
        if method == "capture_frame":
            return {"result": {"directory": self.capture_dir, "entries_total": 1}}
        if method == "read_memory":
            return {"result": {"address": request["params"]["address"], "hex": "0000803f"}}
        if method == "read_gpu_memory":
            return {"error": "GPU address 0x5000 is not mapped"}
        if method in ("input", "input_sequence"):
            return {"result": {"controller": "player1", "frames": 3}}
        if method == "write_memory":
            return {"error": "write_memory is off: start the emulator with "
                             "EDEN_NX2_DEBUG_WRITE=1 to allow it"}
        return {"error": f"unknown method \"{method}\""}

    def serve(self):
        while True:
            try:
                connection, _ = self.listener.accept()
            except OSError:
                return
            with connection, connection.makefile("rwb") as stream:
                for line in stream:
                    request = json.loads(line)
                    self.requests.append(request)
                    response = {"id": request["id"], **self.answer(request)}
                    stream.write(json.dumps(response).encode() + b"\n")
                    stream.flush()

    def close(self):
        self.listener.close()


class AdapterTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.capture_dir = os.path.join(self.temp.name, "capture-1")
        os.mkdir(self.capture_dir)
        with open(os.path.join(self.capture_dir, "0001_present.png"), "wb") as file:
            file.write(b"\x89PNG present")
        self.fake = FakeEmulator(self.capture_dir)
        self.adapter = debug_mcp.Adapter(debug_mcp.Emulator(f"127.0.0.1:{self.fake.port}"))

    def tearDown(self):
        self.adapter.emulator.close()
        self.fake.close()
        self.temp.cleanup()

    def call(self, name, arguments=None):
        reply = self.adapter.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                     "params": {"name": name, "arguments": arguments or {}}})
        return reply["result"]

    def test_initialize_and_list(self):
        reply = self.adapter.handle({"jsonrpc": "2.0", "id": 0, "method": "initialize",
                                     "params": {"protocolVersion": "2025-03-26"}})
        self.assertEqual(reply["result"]["protocolVersion"], "2025-03-26")
        self.assertIn("tools", reply["result"]["capabilities"])
        newer = self.adapter.handle({"id": 0, "method": "initialize",
                                     "params": {"protocolVersion": "2099-01-01"}})
        self.assertEqual(newer["result"]["protocolVersion"], debug_mcp.PROTOCOL_VERSIONS[0])
        self.assertIsNone(self.adapter.handle({"method": "notifications/initialized"}))
        names = [tool["name"] for tool in self.adapter.handle({"id": 2, "method": "tools/list"})
                 ["result"]["tools"]]
        self.assertEqual(names, ["status", "screenshot", "log_tail", "log_filter", "capture_frame",
                                 "capture_image", "reject_shaders", "sample", "read_memory",
                                 "search_memory", "write_memory", "read_gpu_memory", "input",
                                 "input_sequence"])
        self.assertEqual(self.adapter.handle({"id": 3, "method": "bogus"})["error"]["code"], -32601)

    def test_status_is_forwarded(self):
        result = self.call("status", {"fps_window_ms": 0})
        self.assertFalse(result["isError"])
        self.assertEqual(json.loads(result["content"][0]["text"])["title"], "Test")
        self.assertEqual(self.fake.requests[-1]["params"], {"fps_window_ms": 0})

    def test_memory_reads_are_forwarded(self):
        arguments = {"address": "0x8000001000", "size": 4, "as": "hex"}
        result = self.call("read_memory", arguments)
        self.assertFalse(result["isError"])
        self.assertEqual(json.loads(result["content"][0]["text"])["hex"], "0000803f")
        self.assertEqual(self.fake.requests[-1]["params"], arguments)
        unmapped = self.call("read_gpu_memory", {"gpu_address": "0x5000", "size": 4})
        self.assertTrue(unmapped["isError"])
        self.assertIn("0x5000 is not mapped", unmapped["content"][0]["text"])

    def test_write_memory_is_forwarded_and_its_refusal_shown(self):
        arguments = {"address": "0x8000001000", "f32": [1.0]}
        result = self.call("write_memory", arguments)
        self.assertTrue(result["isError"])
        self.assertIn("EDEN_NX2_DEBUG_WRITE=1", result["content"][0]["text"])
        self.assertEqual(self.fake.requests[-1]["params"], arguments)

    def test_input_waits_as_long_as_the_emulator(self):
        self.assertEqual(debug_mcp.default_timeout("input", {"frames": 600}), 40)
        sequence = {"steps": [{"frames": 300}, {"frames": 300, "buttons": ["A"]}]}
        self.assertEqual(debug_mcp.default_timeout("input_sequence", sequence), 40)
        self.assertEqual(debug_mcp.default_timeout("status", {}), 5)
        result = self.call("input_sequence", sequence)
        self.assertFalse(result["isError"])
        self.assertEqual(self.fake.requests[-1]["params"], sequence)

    def test_screenshot_becomes_image_content(self):
        result = self.call("screenshot")
        self.assertEqual(result["content"][0], {"type": "image", "data": PNG,
                                                "mimeType": "image/png"})
        self.assertNotIn("png_base64", result["content"][1]["text"])

    def test_emulator_errors_are_tool_errors(self):
        result = self.call("log_filter", {"filter": "*:Debug"})
        self.assertTrue(result["isError"])
        self.assertIn("unknown method", result["content"][0]["text"])

    def test_capture_image_only_reads_this_sessions_captures(self):
        self.assertTrue(self.call("capture_image", {"directory": self.capture_dir,
                                                    "name": "0001_present"})["isError"])
        self.assertFalse(self.call("capture_frame")["isError"])
        image = self.call("capture_image", {"directory": self.capture_dir, "name": "0001_present"})
        self.assertFalse(image["isError"])
        self.assertEqual(base64.b64decode(image["content"][0]["data"]), b"\x89PNG present")
        escape = self.call("capture_image", {"directory": self.capture_dir, "name": "../../etc"})
        self.assertTrue(escape["isError"])

    def test_restarted_emulator_is_reconnected(self):
        self.call("status")
        self.adapter.emulator.sock.close()
        self.adapter.emulator.sock = socket.create_connection(("127.0.0.1", self.fake.port))
        self.adapter.emulator.sock.close()
        self.adapter.emulator.sock = None
        self.assertFalse(self.call("status")["isError"])

    def test_missing_emulator_explains_how_to_start_it(self):
        self.fake.close()
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()
        adapter = debug_mcp.Adapter(debug_mcp.Emulator(f"127.0.0.1:{port}"))
        reply = adapter.handle({"id": 1, "method": "tools/call",
                                "params": {"name": "status", "arguments": {}}})
        self.assertTrue(reply["result"]["isError"])
        self.assertIn("EDEN_NX2_DEBUG_SERVER", reply["result"]["content"][0]["text"])

    def test_stdio_framing(self):
        stdin = io.StringIO('{"jsonrpc":"2.0","id":1,"method":"ping"}\n\nnot json\n')
        stdout = io.StringIO()
        debug_mcp.serve(self.adapter, stdin, stdout)
        lines = [json.loads(line) for line in stdout.getvalue().splitlines()]
        self.assertEqual(lines[0], {"jsonrpc": "2.0", "id": 1, "result": {}})
        self.assertEqual(lines[1]["error"]["code"], -32700)

    def test_runs_as_a_process(self):
        done = subprocess.run([sys.executable, "-I", os.path.join(HERE, "debug_mcp.py"),
                               "--server", f"127.0.0.1:{self.fake.port}"],
                              input='{"jsonrpc":"2.0","id":5,"method":"tools/call","params":'
                                    '{"name":"status","arguments":{}}}\n',
                              capture_output=True, text=True, timeout=30)
        self.assertEqual(done.returncode, 0, done.stderr)
        reply = json.loads(done.stdout)
        self.assertEqual(reply["id"], 5)
        self.assertFalse(reply["result"]["isError"])


    def test_call_once_saves_the_screenshot(self):
        out = os.path.join(self.temp.name, "shot.png")
        stdout = io.StringIO()
        code = debug_mcp.call_once(self.adapter, '{"method":"screenshot","params":{"height":360}}',
                                   out, stdout)
        self.assertEqual(code, 0)
        result = json.loads(stdout.getvalue())
        self.assertEqual(result["width"], 1280)
        self.assertNotIn("png_base64", result)
        self.assertEqual(result["png"], os.path.abspath(out).replace("\\", "/"))
        with open(out, "rb") as file:
            self.assertEqual(file.read(), PNG_BYTES)
        self.assertEqual(self.fake.requests[-1]["params"], {"height": 360})

    def test_call_once_without_out_says_where_the_png_went(self):
        stdout = io.StringIO()
        self.assertEqual(debug_mcp.call_once(self.adapter, '{"method":"screenshot"}', None, stdout), 0)
        self.assertIn("--out", json.loads(stdout.getvalue())["png"])

    def test_call_once_reports_errors_as_json(self):
        for request in ('{"method":"log_filter","params":{"filter":"*:Debug"}}', '{"params":{}}',
                        'not json', '{"method":"bogus"}', '[1]'):
            stdout = io.StringIO()
            self.assertEqual(debug_mcp.call_once(self.adapter, request, None, stdout), 1, request)
            self.assertIn("error", json.loads(stdout.getvalue()), request)

    def test_call_runs_from_the_command_line(self):
        out = os.path.join(self.temp.name, "cli.png")
        script = os.path.join(HERE, "debug_mcp.py")
        server = f"127.0.0.1:{self.fake.port}"
        done = subprocess.run([sys.executable, "-I", script, "--server", server, "--call",
                               '{"method":"screenshot"}', "--out", out],
                              capture_output=True, text=True, timeout=30)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(json.loads(done.stdout)["height"], 720)
        self.assertTrue(os.path.isfile(out))
        failed = subprocess.run([sys.executable, "-I", script, "--server", server, "--call",
                                 '{"method":"log_filter","params":{"filter":"x"}}'],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(failed.returncode, 1)
        self.assertIn("unknown method", json.loads(failed.stdout)["error"])


if __name__ == "__main__":
    unittest.main()
