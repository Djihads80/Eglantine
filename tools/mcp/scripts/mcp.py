"""Tiny MCP client for the emulator: python mcp.py <tool> [json-args] [--save-image path]
Keeps the session id in a temp file and re-initializes on 404."""
import base64, json, os, sys, tempfile, urllib.request, urllib.error

URL = "http://127.0.0.1:6544/mcp"
SID_FILE = os.path.join(tempfile.gettempdir(), "eglantine_mcp_sid.txt")


def post(body, sid=None):
    req = urllib.request.Request(URL, data=json.dumps(body).encode(), method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Accept", "application/json, text/event-stream")
    if sid:
        req.add_header("Mcp-Session-Id", sid)
    with urllib.request.urlopen(req, timeout=180) as r:
        return r.status, dict(r.headers), r.read().decode() or "{}"


def init():
    _, headers, _ = post({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"protocolVersion": "2025-06-18",
                                     "clientInfo": {"name": "claude-code-script"}}})
    sid = headers.get("Mcp-Session-Id") or headers.get("mcp-session-id")
    post({"jsonrpc": "2.0", "method": "notifications/initialized"}, sid)
    open(SID_FILE, "w").write(sid)
    return sid


# Read-only tools are safe to repeat, so they retry if the connection is reset mid-reply
# (some security software interferes with loopback connections).
RETRY_SAFE = {"status", "screenshot", "screenshot_raw", "capture_frames", "screen_state",
              "timeline", "gpu_profile", "summarize_since", "game_info", "read_memory",
              "list_threads", "process_info", "memory_map", "probe_read", "route_list",
              "tas_status", "get_controller_state", "unimplemented_commands", "ipc_summary"}


def call(tool, args):
    sid = open(SID_FILE).read().strip() if os.path.exists(SID_FILE) else init()
    body = {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
            "params": {"name": tool, "arguments": args}}
    for attempt in range(3 if tool in RETRY_SAFE else 1):
        try:
            return json.loads(post(body, sid)[2])
        except urllib.error.HTTPError as e:
            if e.code in (400, 404):
                sid = init()
                continue
            raise
        except ConnectionResetError:
            if tool == "route_run" and not args.get("background"):
                # The route ran on the server; only the reply was cut. Report its outcome.
                status = call("route_status", {})
                status.setdefault("result", {})["note"] = "reply was reset; this is route_status"
                return status
            if attempt == 2 or tool not in RETRY_SAFE:
                raise
    return json.loads(post(body, sid)[2])


if __name__ == "__main__":
    tool = sys.argv[1]
    args = json.loads(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else {}
    save = sys.argv[sys.argv.index("--save-image") + 1] if "--save-image" in sys.argv else None
    resp = call(tool, args)
    if "error" in resp:
        print("JSON-RPC ERROR:", resp["error"])
        sys.exit(1)
    result = resp["result"]
    for block in result.get("content", []):
        if block["type"] == "text" and "structuredContent" not in result:
            print(block["text"])
        elif block["type"] == "image" and save:
            open(save, "wb").write(base64.b64decode(block["data"]))
            print(f"[image saved to {save}]")
    if "structuredContent" in result:
        print(json.dumps(result["structuredContent"], indent=1)[:6000])
    if result.get("isError"):
        sys.exit(2)
