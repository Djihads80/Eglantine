#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Live shader lab for Switch 2 titles: browse every vertex, pixel and compute program the running
emulator sees, hide them one by one (or in bulk, to bisect), watch which constant buffer words
change from frame to frame, and read the disassembly.

Run the emulator with EDEN_NX2_DIAG_DIR=<dir> (and EDEN_NX2_SHADER_DUMP=<dumps> for disassembly,
EDEN_NX2_DIAG_SAMPLES=1 for the per-draw pixel probe), then:

    python3 -I tools/nx2/shader_lab.py --diag-dir <dir> [--dump-dir <dumps>] [--frames <frames>]

and open the address it prints (also in <dir>/lab-url.txt). With EDEN_NX2_LAB=1 the emulator starts
it for you (EDEN_NX2_LAB=open also opens the browser) and it exits with the emulator. The emulator writes <dir>/diag-state.json once a second and re-reads
<dir>/ablate.txt and <dir>/inspect.txt when they change; this tool only edits those files (and
labels.json). Nothing here touches the game's own files. Programs are identified by their dump key,
a hash of the code, which stays the same between runs (addresses do not).

What each program draws goes into the game's shader catalogue (catalog.py): the repo copy in
tools/nx2/catalog/<program id>.json merged with a local copy that carries over between builds
(--catalog-dir, EDEN_NX2_CATALOG_DIR). Edits are saved to the local copy; `catalog.py status` and
`catalog.py merge` bring them into the repo copy before a commit.
"""

import argparse
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

KEY = re.compile(r"^[0-9a-f]{16}$")
HERE = os.path.dirname(os.path.abspath(__file__))


def load_catalog_module():
    """tools/nx2/catalog.py, loaded by path (python -I leaves this folder out of sys.path)."""
    spec = importlib.util.spec_from_file_location("nx2_catalog", os.path.join(HERE, "catalog.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CATALOG = load_catalog_module()


class Lab:
    def __init__(self, diag_dir, dump_dir, frames_dir, catalog_dir=None, title=None):
        self.dir = diag_dir
        self.dump_dir = dump_dir
        self.frames_dir = frames_dir
        self.lock = threading.Lock()
        self.dumps = {}
        self.disassembly = {}
        self.catalog_dir = catalog_dir or CATALOG.local_dir()
        self.title_override = CATALOG.normalize_title(title)
        self.title = self.title_override
        self.labels_imported = False

    def path(self, name):
        return os.path.join(self.dir, name)

    def read_lines(self, name):
        try:
            with open(self.path(name), encoding="utf-8") as file:
                return file.read().splitlines()
        except OSError:
            return []

    def write_lines(self, name, lines):
        temporary = self.path(name + ".tmp")
        with open(temporary, "w", encoding="utf-8") as file:
            file.write("\n".join(lines) + ("\n" if lines else ""))
        os.replace(temporary, self.path(name))

    def keys(self, name):
        found = []
        for line in self.read_lines(name):
            word = line.split("#")[0].split()
            if len(word) >= 2 and word[0] == "key" and KEY.match(word[1]):
                found.append(word[1])
            elif len(word) == 1 and KEY.match(word[0]):
                found.append(word[0])
        return found

    def set_key(self, name, key, on, prefix):
        """Add or remove one key from a control file, keeping every other line as it is."""
        with self.lock:
            lines = [line for line in self.read_lines(name)
                     if (line.split("#")[0].split() or [""])[-1] != key]
            if on:
                lines.append(f"{prefix}{key}")
            self.write_lines(name, lines)

    def set_keys(self, name, keys, on, prefix):
        with self.lock:
            drop = set(keys)
            lines = [line for line in self.read_lines(name)
                     if (line.split("#")[0].split() or [""])[-1] not in drop]
            if on:
                lines += [f"{prefix}{key}" for key in keys]
            self.write_lines(name, lines)

    def labels(self):
        try:
            with open(self.path("labels.json"), encoding="utf-8") as file:
                return json.load(file)
        except (OSError, ValueError):
            return {}

    def set_label(self, key, text):
        with self.lock:
            labels = self.labels()
            if text:
                labels[key] = text
            else:
                labels.pop(key, None)
            temporary = self.path("labels.json.tmp")
            with open(temporary, "w", encoding="utf-8") as file:
                json.dump(labels, file, indent=1)
            os.replace(temporary, self.path("labels.json"))

    def catalogue(self, title):
        """The game's catalogue: the repo copy merged with the local one."""
        if title:
            merged = CATALOG.merge(CATALOG.load_title(CATALOG.REPO_DIR, title),
                                   CATALOG.load_title(self.catalog_dir, title))
            merged["title"] = title
            return merged
        # The emulator didn't say which game (older build): show every game's entries.
        names = CATALOG.titles(CATALOG.REPO_DIR, self.catalog_dir)
        merged = CATALOG.merge(*[CATALOG.load_title(folder, name) for name in names
                                 for folder in (CATALOG.REPO_DIR, self.catalog_dir)])
        merged["title"] = ""
        return merged

    def sync_catalogue(self, state):
        """Fills in program keys for entries known only by hash, and imports labels.json once."""
        title = self.title_override or CATALOG.normalize_title(state.get("title"))
        self.title = title
        with self.lock:
            catalog = self.catalogue(title)
            if not title:
                return catalog
            programs = catalog["programs"]
            changed = False
            for program in state.get("programs", []):
                key, hash_value = program.get("key") or "", program.get("hash") or ""
                if not KEY.match(key) or key == "0" * 16 or not KEY.match(hash_value):
                    continue
                if CATALOG.find(programs, key=key) is not None:
                    continue
                entry = CATALOG.find(programs, hash_value=hash_value)
                if entry is not None and not entry.get("key"):
                    entry["key"] = key
                    entry["updated"] = CATALOG.now()
                    changed = True
            if not self.labels_imported:
                self.labels_imported = True
                for key, text in self.labels().items():
                    if KEY.match(key) and text and CATALOG.find(programs, key=key) is None:
                        programs.append({"key": key, "object": str(text), "how": "shader lab label",
                                         "updated": CATALOG.now()})
                        changed = True
            if changed:
                CATALOG.save(CATALOG.path_for(self.catalog_dir, title), catalog)
            return catalog

    def set_catalog_entry(self, key, hash_value, fields):
        title = self.title
        if not title:
            return "The game is not known yet: start the lab with --title <program id>."
        entry = {"key": key, "hash": hash_value, "updated": CATALOG.now()}
        for field in CATALOG.TEXT_FIELDS:
            entry[field] = str(fields.get(field) or "")[:2000]
        entry = CATALOG.clean(entry)
        with self.lock:
            catalog = CATALOG.merge(self.catalogue(title), {"programs": [entry]})
            catalog["title"] = title
            CATALOG.save(CATALOG.path_for(self.catalog_dir, title), catalog)
        return None

    def state(self):
        try:
            with open(self.path("diag-state.json"), encoding="utf-8") as file:
                state = json.load(file)
        except (OSError, ValueError):
            state = {"frame": 0, "programs": [], "ablated": [], "inspect": []}
        state["ablated"] = self.keys("ablate.txt")
        state["inspected"] = self.keys("inspect.txt")
        state["labels"] = self.labels()
        state["dumps"] = len(self.scan_dumps())
        catalog = self.sync_catalogue(state)
        upload = 0
        if self.title:
            new, updated, _ = CATALOG.compare(CATALOG.load_title(self.catalog_dir, self.title),
                                              CATALOG.load_title(CATALOG.REPO_DIR, self.title))
            upload = len(new) + len(updated)
        state["catalog"] = {
            "title": self.title, "game": catalog.get("game", ""), "entries": catalog["programs"],
            "upload": upload,
            "local": CATALOG.path_for(self.catalog_dir, self.title or "<program id>"),
            "repo": CATALOG.path_for(CATALOG.REPO_DIR, self.title or "<program id>"),
        }
        return state

    def scan_dumps(self, force=False):
        if not self.dump_dir:
            return {}
        if force or not self.dumps:
            found = {}
            try:
                for name in os.listdir(self.dump_dir):
                    match = re.match(r"^([0-9a-f]{16})-(gfx|cs)-[0-9a-f]+\.bin$", name)
                    if match:
                        found[match.group(1)] = name
            except OSError:
                pass
            self.dumps = found
        return self.dumps

    def disassemble(self, key):
        if key in self.disassembly:
            return self.disassembly[key]
        name = self.scan_dumps().get(key) or self.scan_dumps(force=True).get(key)
        if not name:
            return "No dump for this program: run the game with EDEN_NX2_SHADER_DUMP and pass --dump-dir."
        cache = self.path(os.path.join("dis", key + ".txt"))
        if os.path.exists(cache):
            with open(cache, encoding="utf-8") as file:
                text = file.read()
        else:
            tool = os.path.join(HERE, "nvd_cubin.py")
            try:
                result = subprocess.run([sys.executable, "-I", tool, os.path.join(self.dump_dir, name)],
                                        capture_output=True, text=True, timeout=120)
                text = result.stdout or result.stderr or "(empty disassembly)"
            except (OSError, subprocess.SubprocessError) as error:
                return f"Could not run nvd_cubin.py: {error}"
            os.makedirs(os.path.dirname(cache), exist_ok=True)
            with open(cache, "w", encoding="utf-8") as file:
                file.write(text)
        self.disassembly[key] = text
        return text

    def latest_frame(self):
        """The most recently written frame capture as PNG bytes (needs ImageMagick), or None."""
        newest = None
        # The screenshots of "Capture frame" (capture-<n>.tga, in the diag folder) and, if the
        # emulator also ran with EDEN_NX2_FRAME_CAPTURE, its periodic frames.
        for folder in (self.dir, self.frames_dir):
            if not folder:
                continue
            try:
                with os.scandir(folder) as entries:
                    for entry in entries:
                        if entry.name.endswith(".tga") and entry.is_file(follow_symlinks=False):
                            stamp = entry.stat(follow_symlinks=False)
                            if newest is None or stamp.st_mtime_ns > newest[0].st_mtime_ns:
                                newest = (stamp, entry.path)
            except OSError:
                continue
        if newest is None:
            return None
        stamp, source = newest
        # Frame names repeat from run to run, so the cache key includes when the file was written.
        key = hashlib.sha1(f"{source}:{stamp.st_mtime_ns}:{stamp.st_size}".encode()).hexdigest()[:12]
        target = self.path(os.path.join("preview", key + ".png"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        if not os.path.exists(target):
            try:
                subprocess.run(["magick", source, "-resize", "960x", target], check=True, timeout=30)
            except (OSError, subprocess.SubprocessError):
                return None
            for name in os.listdir(os.path.dirname(target)):
                if name != key + ".png":
                    try:
                        os.unlink(os.path.join(os.path.dirname(target), name))
                    except OSError:
                        pass
        with open(target, "rb") as file:
            return file.read()

    # What each "clear" button deletes: only files this workflow creates, never subfolders the user
    # made, and never the toggles (ablate.txt, inspect.txt) or labels in the diag folder.
    CLEARABLE = {
        "diag": (r"^(draw-list-\d+\.tsv|capture-\d+\.tga|diag-state\.json(\.tmp)?|lab\.log|draw-list)$", ("dis", "preview")),
        "dumps": (r"^[0-9a-f]{16}-(gfx|cs)-[0-9a-f]+\.bin$|^rejected-[0-9a-f]+\.bin$", ()),
        "frames": (r"^nx2-(frame-\d+\.tga|dump-[0-9a-f]+\.bin)$", ()),
    }

    def folder(self, what):
        return {"diag": self.dir, "dumps": self.dump_dir, "frames": self.frames_dir}.get(what)

    def clear_plan(self, what):
        """(folder, [files], [lab-owned subfolders]) a clear would remove, or an error string."""
        folder = self.folder(what)
        if what not in self.CLEARABLE:
            return "unknown folder"
        if not folder:
            return {"diag": "no diag folder", "dumps": "start the lab with --dump-dir",
                    "frames": "start the lab with --frames"}[what]
        folder = os.path.realpath(folder)
        parts = [part for part in folder.split(os.sep) if part]
        if len(parts) < 3 or folder == os.path.realpath(os.path.expanduser("~")):
            return f"refusing to clear {folder}"
        if not os.path.isdir(folder):
            return f"{folder} does not exist"
        pattern, subfolders = self.CLEARABLE[what]
        files = []
        with os.scandir(folder) as entries:
            for entry in entries:
                if entry.is_file(follow_symlinks=False) and re.match(pattern, entry.name):
                    files.append(entry.path)
        return folder, files, [os.path.join(folder, name) for name in subfolders
                               if os.path.isdir(os.path.join(folder, name))
                               and not os.path.islink(os.path.join(folder, name))]

    def clear_info(self, what):
        plan = self.clear_plan(what)
        if isinstance(plan, str):
            return {"error": plan}
        folder, files, subfolders = plan
        size = 0
        for path in files:
            try:
                size += os.path.getsize(path)
            except OSError:
                pass
        return {"path": folder, "files": len(files), "bytes": size}

    def clear(self, what):
        plan = self.clear_plan(what)
        if isinstance(plan, str):
            return {"error": plan}
        folder, files, subfolders = plan
        removed = 0
        for path in files:
            try:
                os.unlink(path)
                removed += 1
            except OSError:
                pass
        for path in subfolders:
            for name in os.listdir(path):
                try:
                    os.unlink(os.path.join(path, name))
                except OSError:
                    pass
        self.dumps = {}
        self.disassembly = {}
        return {"path": folder, "removed": removed}


def process_alive(pid):
    """Whether a process still runs. On Windows os.kill(pid, 0) would send it CTRL_C_EVENT."""
    if os.name == "nt":
        import ctypes
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return False
        code = ctypes.c_ulong()
        ok = kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
        kernel32.CloseHandle(handle)
        return bool(ok) and code.value == 259  # STILL_ACTIVE
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


class Handler(BaseHTTPRequestHandler):
    lab = None

    def log_message(self, *args):
        pass

    def send(self, body, kind="application/json", status=200):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlparse(self.path)
        query = parse_qs(url.query)
        if url.path == "/":
            self.send(PAGE, "text/html; charset=utf-8")
        elif url.path == "/api/state":
            self.send(json.dumps(self.lab.state()))
        elif url.path == "/api/disasm":
            key = (query.get("key") or [""])[0]
            if not KEY.match(key):
                return self.send("bad key", "text/plain", 400)
            self.send(self.lab.disassemble(key), "text/plain; charset=utf-8")
        elif url.path == "/api/clearinfo":
            self.send(json.dumps(self.lab.clear_info((query.get("what") or [""])[0])))
        elif url.path == "/api/frame":
            png = self.lab.latest_frame()
            if png is None:
                return self.send("no frame", "text/plain", 404)
            self.send(png, "image/png")
        else:
            self.send("not found", "text/plain", 404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            return self.send("bad json", "text/plain", 400)
        keys = [key for key in ([body.get("key")] + list(body.get("keys") or [])) if key]
        if any(not isinstance(key, str) or not KEY.match(key) for key in keys):
            return self.send("bad key", "text/plain", 400)
        if self.path == "/api/ablate":
            self.lab.set_keys("ablate.txt", keys, bool(body.get("on")), "key ")
        elif self.path == "/api/inspect":
            self.lab.set_keys("inspect.txt", keys, bool(body.get("on")), "")
        elif self.path == "/api/label" and keys:
            self.lab.set_label(keys[0], str(body.get("label") or "")[:80])
            if self.lab.title:
                self.lab.set_catalog_entry(keys[0], "", {"object": str(body.get("label") or "")[:80]})
        elif self.path == "/api/catalog" and keys:
            hash_value = str(body.get("hash") or "")
            if hash_value and not KEY.match(hash_value):
                return self.send("bad hash", "text/plain", 400)
            fields = body.get("fields") if isinstance(body.get("fields"), dict) else {}
            error = self.lab.set_catalog_entry(keys[0], hash_value, fields)
            return self.send(json.dumps({"error": error} if error else {}))
        elif self.path == "/api/clear":
            return self.send(json.dumps(self.lab.clear(str(body.get("what") or ""))))
        elif self.path == "/api/capture":
            open(self.lab.path("draw-list"), "a").close()
        else:
            return self.send("not found", "text/plain", 404)
        self.send("{}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--diag-dir", required=True, help="the emulator's EDEN_NX2_DIAG_DIR")
    parser.add_argument("--dump-dir", help="the emulator's EDEN_NX2_SHADER_DUMP (for disassembly)")
    parser.add_argument("--frames", help="optional: the emulator's EDEN_NX2_FRAME_CAPTURE folder, whose periodic "
                        "frames the preview may also show (\"Capture frame\" saves its own screenshot)")
    parser.add_argument("--port", type=int, default=8765, help="first port to try (the next 19 are tried if busy)")
    parser.add_argument("--parent-pid", type=int, help="exit when this process (the emulator) is gone")
    parser.add_argument("--open", action="store_true", help="open the page in the default browser")
    parser.add_argument("--catalog-dir", help="local shader catalogue folder (default: EDEN_NX2_CATALOG_DIR, "
                        "else the per-user folder catalog.py uses)")
    parser.add_argument("--title", help="program id of the game, for emulator builds that don't report it")
    args = parser.parse_args()
    os.makedirs(args.diag_dir, exist_ok=True)
    Handler.lab = Lab(args.diag_dir, args.dump_dir, args.frames, args.catalog_dir, args.title)
    server = None
    for port in range(args.port, args.port + 20):
        try:
            server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
            break
        except OSError:
            continue
    if server is None:
        sys.exit(f"No free port from {args.port} to {args.port + 19}")
    url = f"http://127.0.0.1:{server.server_address[1]}"
    with open(os.path.join(args.diag_dir, "lab-url.txt"), "w", encoding="utf-8") as file:
        file.write(url + "\n")
    print(f"Shader lab on {url} (diag dir {args.diag_dir})", flush=True)
    if args.parent_pid:
        def watch():
            while True:
                time.sleep(3)
                if not process_alive(args.parent_pid):
                    os._exit(0)
        threading.Thread(target=watch, daemon=True).start()
    if args.open:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Shader lab</title>
<style>
:root{--bg:#fff;--fg:#1d2024;--mut:#667;--line:#d8dce2;--acc:#2563eb;--bad:#c0392b;--ok:#1e8449;--hi:#eef3ff;--code:#f5f6f8}
@media (prefers-color-scheme:dark){:root{--bg:#14161a;--fg:#e4e6ea;--mut:#98a0ad;--line:#2c313a;--acc:#6ea0ff;--bad:#ff7b6b;--ok:#5fd08a;--hi:#1c2638;--code:#1b1e24}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:13px/1.4 system-ui,sans-serif;height:100vh;display:flex;flex-direction:column}
header{display:flex;gap:10px;align-items:center;padding:8px 12px;border-bottom:1px solid var(--line);flex-wrap:wrap}
header b{font-size:14px}.sp{flex:1}
button,input,select{font:inherit;color:inherit;background:var(--bg);border:1px solid var(--line);border-radius:5px;padding:3px 8px}
button{cursor:pointer}button:hover{border-color:var(--acc)}button.on{background:var(--acc);color:#fff;border-color:var(--acc)}
main{flex:1;display:flex;min-height:0}
#left{flex:1.3;min-width:0;display:flex;flex-direction:column;border-right:1px solid var(--line)}
#tools{display:flex;gap:6px;padding:8px 12px;flex-wrap:wrap;align-items:center;border-bottom:1px solid var(--line)}
#list{flex:1;overflow:auto}
table{border-collapse:collapse;width:100%}th,td{padding:3px 8px;text-align:right;white-space:nowrap}
th{position:sticky;top:0;background:var(--bg);border-bottom:1px solid var(--line);cursor:pointer;user-select:none;font-weight:600}
td.l,th.l{text-align:left}tr.row{cursor:pointer}tr.row:hover{background:var(--hi)}tr.sel{background:var(--hi)}
.hid{color:var(--bad);font-weight:600}.mono{font-family:ui-monospace,monospace}.mut{color:var(--mut)}
#right{flex:1;min-width:0;overflow:auto;padding:10px 14px}
h3{margin:.2em 0}.tabs{display:flex;gap:4px;margin:10px 0}
pre{background:var(--code);padding:8px;border-radius:6px;overflow:auto;max-height:60vh;font-size:12px}
.k{color:var(--acc)}.w{color:var(--bad)}img{max-width:100%;border-radius:6px;border:1px solid var(--line)}
.note{padding:6px 8px;border-left:3px solid var(--acc);background:var(--hi);margin:8px 0;border-radius:3px}
@media (max-width:800px){main{flex-direction:column}#left{border-right:0;border-bottom:1px solid var(--line)}}
.cat{border:1px solid var(--line);border-radius:6px;padding:8px;margin:8px 0}
.cat label{display:inline-flex;gap:4px;align-items:center;margin:3px 10px 3px 0}
.cat textarea{width:100%;font:inherit;color:inherit;background:var(--bg);border:1px solid var(--line);border-radius:5px;padding:4px;min-height:3.4em;margin:4px 0}
.badge{font-size:11px;padding:1px 7px;border-radius:9px;border:1px solid var(--line);color:var(--mut)}
#catmeta.up{color:var(--bad);font-weight:600}
</style></head><body>
<header><b>Shader lab</b><span id="meta" class="mut"></span><span id="catmeta" class="mut"></span><span class="sp"></span>
<button id="cap" title="Write the next frame's draw list">Capture frame</button>
<button data-clear="diag" title="Delete draw lists, the state file and caches (keeps hidden/inspect lists and labels)">Clear diag</button>
<button data-clear="dumps" title="Delete the shader dump files">Clear dumps</button>
<button data-clear="frames" title="Delete the frame captures">Clear frames</button></header>
<main>
<div id="left"><div id="tools">
 <span id="kinds"></span>
 <input id="q" placeholder="filter key / address / label" size="22">
 <label><input type="checkbox" id="act" checked> active</label>
 <label><input type="checkbox" id="hon"> hidden only</label>
 <label><input type="checkbox" id="ins"> inspected only</label>
 <button id="catv" title="List every catalogue entry of this game, also programs not seen in this session">catalogue</button>
 <span class="sp"></span>
 <button id="hhalf" title="Hide the first half of the rows shown (bisect)">Hide ½</button>
 <button id="hall">Hide shown</button><button id="uall">Unhide all</button>
</div><div id="list"></div></div>
<div id="right"><div id="detail" class="mut">Select a program.</div></div>
</main>
<script>
const $=s=>document.querySelector(s);
const KIND={v:"vertex",p:"pixel",c:"compute"};
let S={programs:[],ablated:[],inspected:[],inspect:[],labels:{},frame:0,dumps:0};
let sel=null,tab="over",sortBy="per_s",sortDir=-1,kinds=new Set(["v","c"]),shown=[],disCache={};
let catView=false,editing=false;
const ents=()=>(S.catalog&&S.catalog.entries)||[];
function entryFor(p){const e=ents();return e.find(x=>x.key&&x.key===p.key)||(p.hash?e.find(x=>x.hash===p.hash&&(!x.key||x.key===p.key)):null)||null}
const nameOf=p=>{const e=entryFor(p);return (e&&e.object)||S.labels[p.key]||""};
const inCat=()=>{const a=document.activeElement;return !!(a&&a.closest&&a.closest(".cat"))};
const post=(u,b)=>fetch(u,{method:"POST",body:JSON.stringify(b)}).then(()=>refresh());
const esc=s=>String(s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;"}[c]));
const num=n=>n>=1e6?(n/1e6).toFixed(1)+"M":n>=1e4?(n/1e3).toFixed(0)+"k":n;
const isHid=p=>S.ablated.includes(p.key);
const isIns=p=>S.inspected.includes(p.key);
function buildKinds(){$("#kinds").innerHTML=Object.entries(KIND).map(([k,n])=>`<button data-k="${k}" class="${kinds.has(k)?"on":""}">${n}</button>`).join(" ")}
$("#kinds").onclick=e=>{const k=e.target.dataset.k;if(!k)return;kinds.has(k)?kinds.delete(k):kinds.add(k);buildKinds();render()};
async function refresh(){try{S=await (await fetch("/api/state")).json()}catch(e){return}
 $("#meta").textContent=`frame ${S.frame} · ${S.programs.length} programs · ${S.ablated.length} hidden · ${S.dumps} dumps`;
 const c=S.catalog||{},cm=$("#catmeta");
 cm.textContent=c.entries?` · catalogue ${c.entries.length}${c.title?"":" (game unknown)"}${c.upload?` · ${c.upload} to upload`:""}`:"";
 cm.className=c.upload?"up":"mut";
 cm.title=c.local?(c.upload?"Entries in your local catalogue that the repo copy lacks. Before committing run\npython3 -I tools/nx2/catalog.py status, then catalog.py merge, and commit the JSON.\n":"")+`local: ${c.local}\nrepo: ${c.repo}`:"";
 render();if(sel&&!editing&&!inCat())drawDetail()}
function rows(){const q=$("#q").value.toLowerCase();
 return S.programs.filter(p=>kinds.has(p.kind)&&(!$("#act").checked||S.frame-p.last_frame<180)
  &&(!$("#hon").checked||isHid(p))&&(!$("#ins").checked||isIns(p))
  &&(!q||(p.key+" "+p.addr+" "+(p.hash||"")+" "+nameOf(p)+" "+((entryFor(p)||{}).group||"")+" "+((entryFor(p)||{}).notes||"")).toLowerCase().includes(q)))
  .sort((a,b)=>{const x=a[sortBy],y=b[sortBy];return (x>y?1:x<y?-1:0)*sortDir})}
function renderCat(){const q=$("#q").value.toLowerCase(),seen=new Map();
 for(const p of S.programs){seen.set(p.key,p);if(p.hash)seen.set("h"+p.hash,p)}
 const prog=e=>(e.key&&seen.get(e.key))||(e.hash&&seen.get("h"+e.hash))||null;
 const list=ents().filter(e=>!q||[e.key,e.hash,e.object,e.group,e.notes,e.stage,e.how].join(" ").toLowerCase().includes(q))
  .map(e=>[e,prog(e)]).sort((a,b)=>(b[1]?1:0)-(a[1]?1:0));
 $("#list").innerHTML="<table><tr><th class=l>seen</th><th class=l>stage</th><th class=l>key</th><th class=l>hash</th><th class=l>object</th><th class=l>group</th></tr>"+
 list.map(([e,p])=>`<tr class="row ${p&&sel===p.key+p.kind?"sel":""}" ${p?`data-id="${p.key}${p.kind}"`:""} title="${esc([e.key&&"key "+e.key,e.hash&&"hash "+e.hash,e.how,e.notes].filter(Boolean).join(" · "))}"><td class=l>${p?"●":""}</td><td class=l>${esc(e.stage||"")}</td><td class="l mono">${(e.key||"").slice(0,10)}</td><td class="l mono mut">${(e.hash||"").slice(0,10)}</td><td class=l>${esc(e.object||"")}</td><td class="l mut">${esc(e.group||"")}</td></tr>`).join("")+"</table>"}
function render(){if(catView){shown=[];renderCat();return}
 shown=rows();const cols=[["kind","l"],["key","l"],["addr","l"],["per_s",""],["draws",""],["instances",""],["vertices",""],["rt",""],["probe",""]];
 $("#list").innerHTML="<table><tr><th class=l></th>"+cols.map(([c,a])=>`<th class="${a}" data-c="${c}">${c==="per_s"?"/s":c}${c===sortBy?(sortDir>0?" ▲":" ▼"):""}</th>`).join("")+"<th class=l>label</th></tr>"+
 shown.slice(0,600).map(p=>`<tr class="row ${sel===p.key+p.kind?"sel":""}" data-id="${p.key}${p.kind}"><td class=l><input type=checkbox data-h="${p.key}" ${isHid(p)?"":"checked"} title="drawn / hidden"></td><td class="l">${KIND[p.kind][0]}</td><td class="l mono">${p.key.slice(0,10)}</td><td class="l mono mut">${p.addr}</td><td>${p.per_s}</td><td>${num(p.draws)}</td><td>${num(p.instances)}</td><td>${num(p.vertices)}</td><td class=mut>${p.rt}</td><td class="${p.probe&&p.probe.zero===p.probe.draws?"hid":"mut"}" title="draws that passed no samples / draws probed in the last capture">${p.probe?p.probe.zero+"/"+p.probe.draws:""}</td><td class="l ${isHid(p)?"hid":""}">${isHid(p)?"hidden ":""}${isIns(p)?"◉ ":""}${esc(nameOf(p))}</td></tr>`).join("")+"</table>"}
$("#list").onclick=e=>{const h=e.target.dataset.h;if(h){post("/api/ablate",{key:h,on:!e.target.checked});return}
 const th=e.target.closest("th");if(th&&th.dataset.c){const c=th.dataset.c;sortDir=c===sortBy?-sortDir:-1;sortBy=c;render();return}
 const tr=e.target.closest("tr.row");if(tr&&tr.dataset.id){sel=tr.dataset.id;editing=false;render();drawDetail()}};
for(const id of ["#q","#act","#hon","#ins"])$(id).oninput=render;
$("#catv").onclick=()=>{catView=!catView;$("#catv").classList.toggle("on",catView);render()};
$("#hall").onclick=()=>post("/api/ablate",{keys:shown.filter(p=>p.key!=="0".repeat(16)).map(p=>p.key),on:true});
$("#hhalf").onclick=()=>post("/api/ablate",{keys:shown.slice(0,Math.ceil(shown.length/2)).filter(p=>p.key!=="0".repeat(16)).map(p=>p.key),on:true});
$("#uall").onclick=()=>post("/api/ablate",{keys:S.ablated,on:false});
$("#cap").onclick=()=>post("/api/capture",{});
for(const b of document.querySelectorAll("[data-clear]"))b.onclick=async()=>{const what=b.dataset.clear;
 const i=await (await fetch("/api/clearinfo?what="+what)).json();
 if(i.error){alert(i.error);return}
 const mb=(i.bytes/1048576).toFixed(1);
 const warn=what==="dumps"?"\n\nThe game writes a dump when it first translates a program, so clear dumps before launching the game, not during a run.":what==="diag"?"\n\nHidden/inspected lists and labels are kept.":"";
 if(!confirm(`Delete ${i.files} file(s), ${mb} MB, from\n${i.path}?${warn}`))return;
 const r=await (await fetch("/api/clear",{method:"POST",body:JSON.stringify({what})})).json();
 alert(r.error||`Removed ${r.removed} file(s).`);disCache={};refresh()};
function cur(){return S.programs.find(p=>p.key+p.kind===sel)}
async function drawDetail(){const p=cur();if(!p){$("#detail").textContent="Not seen yet.";return}
 const e=entryFor(p)||{},stageDef={v:"vertex",p:"pixel",c:"compute"}[p.kind]||"";
 const known=!!(e.object||e.notes);
 const cat=`<div class=cat><b>Catalogue</b> <span class=badge>${known?(e.key?"catalogued":"matched by hash"):"not catalogued"}</span>
 <div><label>Object <input id=c_object size=34 value="${esc(e.object||S.labels[p.key]||"")}" placeholder="what it draws (water, grass…)"></label>
 <label>Stage <select id=c_stage>${["","vertex","mesh","task","geometry","pixel","compute"].map(s=>`<option ${(e.stage||stageDef)===s?"selected":""}>${s}</option>`).join("")}</select></label></div>
 <div><label>Group <input id=c_group size=20 list=c_groups value="${esc(e.group||"")}" placeholder="Foliage / trees"></label>
 <label>How found <input id=c_how size=28 value="${esc(e.how||"")}" placeholder="hidden live: trees go bare"></label></div>
 <datalist id=c_groups>${[...new Set(ents().map(x=>x.group).filter(Boolean))].map(g=>`<option value="${esc(g)}">`).join("")}</datalist>
 <textarea id=c_notes placeholder="notes: inputs, passes, related programs">${esc(e.notes||"")}</textarea>
 <div class=mut>key ${p.key}${p.hash?` · hash ${p.hash}`:""}${e.source?` · ${esc(e.source)}`:""}${e.updated?` · ${esc(e.updated)}`:""}</div>
 <button id=c_save>Save to catalogue</button> <span id=c_msg class=mut></span></div>`;
 let body="";
 if(tab==="over"){body=`<table>${Object.entries(p.outcomes).map(([k,v])=>`<tr><td class=l>${k}</td><td>${num(v)}</td></tr>`).join("")}</table>
  <p class=mut>last seen frame ${p.last_frame} (now ${S.frame}); last target ${p.rt}</p>
  ${p.probe?`<div class=note>Pixel probe (last capture): ${p.probe.draws} draw(s), <b class="${p.probe.zero===p.probe.draws?"w":""}">${p.probe.zero} passed no samples</b>, ${num(p.probe.samples)} samples in total.${p.probe.zero===p.probe.draws?" Nothing this program drew reached the picture.":""}</div>`:`<p class=mut>No pixel probe yet: start the game with EDEN_NX2_DIAG_SAMPLES=1 and press “Capture frame”.</p>`}`}
 if(tab==="uni"){const w=(S.inspect||[]).find(i=>i.key===p.key);
  if(!isIns(p))body=`<div class=note>Not inspected. Press “Inspect” to watch the constant buffers this program is drawn with.</div>`;
  else if(!w||!w.slots.length)body=`<div class=note>Waiting for a draw with this program…</div>`;
  else body=w.slots.map(s=>`<h4>constant buffer ${s.slot} <span class=mut>(${s.words} words, ${s.frames} frames compared)</span></h4>`+
   (s.changing.length?`<table><tr><th class=l>offset</th><th>changed in</th><th>now (hex / float)</th></tr>`+s.changing.map(c=>`<tr><td class="l mono">+0x${c.off.toString(16)}</td><td>${c.changes}/${s.frames}</td><td class=mono>${(c.value>>>0).toString(16)} / ${f32(c.value)}</td></tr>`).join("")+"</table>":
     `<div class=note><b class=w>Nothing changes between frames</b> in this buffer${s.frames<20?" (so far)":""}. If this shader animates, its time or phase value is not being updated.</div>`)+
   (s.per_draw.length?`<p class=mut>${s.per_draw.length} word(s) vary between draws of the same frame: ${s.per_draw.slice(0,24).map(o=>"+0x"+o.toString(16)).join(" ")}</p>`:"")).join("")}
 if(tab==="dis"){if(!(p.key in disCache)){disCache[p.key]="loading…";fetch("/api/disasm?key="+p.key).then(r=>r.text()).then(t=>{disCache[p.key]=t;if(tab==="dis")drawDetail()})}
  body=`<pre>${esc(disCache[p.key]).replace(/\b(MUFU\.\w+|TEX\w*|TLD\w*|LDG\.\w+|STG\.\w+|ATOMG\.\w+|ALD|AST|BAR\.\w+)/g,"<span class=k>$1</span>")}</pre>`}
 $("#detail").innerHTML=`<h3>${KIND[p.kind]} <span class=mono>${p.key}</span></h3>
 <div class=mut>address ${p.addr} · ${p.per_s}/s · ${num(p.draws)} total</div>
 <p><button id="bh" class="${isHid(p)?"on":""}">${isHid(p)?"Hidden: show":"Hide"}</button>
 <button id="bi" class="${isIns(p)?"on":""}">${isIns(p)?"Inspecting":"Inspect"}</button></p>
 ${cat}
 ${p.kind!=="c"?`<img id="fr" src="/api/frame?${Date.now()>>12}" alt="" onerror="this.remove()">`:""}
 <div class="tabs">${[["over","Overview"],["uni","Uniforms"],["dis","Disassembly"]].map(([k,n])=>`<button data-t="${k}" class="${tab===k?"on":""}">${n}</button>`).join("")}</div>${body}`;
 $("#bh").onclick=()=>post("/api/ablate",{key:p.key,on:!isHid(p)});
 $("#bi").onclick=()=>post("/api/inspect",{key:p.key,on:!isIns(p)});
 for(const id of ["c_object","c_stage","c_group","c_how","c_notes"])$("#"+id).oninput=()=>{editing=true};
 $("#c_save").onclick=async()=>{const fields={object:$("#c_object").value,stage:$("#c_stage").value,group:$("#c_group").value,how:$("#c_how").value,notes:$("#c_notes").value};
  let r={};try{r=await (await fetch("/api/catalog",{method:"POST",body:JSON.stringify({key:p.key,hash:p.hash||"",fields})})).json()}catch(x){r={error:String(x)}}
  editing=false;document.activeElement.blur();await refresh();drawDetail();const m=$("#c_msg");if(m)m.textContent=r.error||"saved to your local catalogue"};
 for(const b of document.querySelectorAll("[data-t]"))b.onclick=()=>{tab=b.dataset.t;drawDetail()}}
function f32(i){const b=new DataView(new ArrayBuffer(4));b.setUint32(0,i>>>0);const v=b.getFloat32(0);return Number.isFinite(v)&&Math.abs(v)<1e9?+v.toPrecision(6):"-"}
buildKinds();refresh();setInterval(refresh,1000);
</script></body></html>
"""

if __name__ == "__main__":
    main()
