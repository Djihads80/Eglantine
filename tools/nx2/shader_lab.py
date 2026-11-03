#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Live shader lab for Switch 2 titles: browse every vertex, pixel and compute program the running
emulator sees, hide them one by one (or in bulk, to bisect), watch which constant buffer words
change from frame to frame, and read the disassembly.

Run the emulator with EDEN_NX2_DIAG_DIR=<dir> (and EDEN_NX2_SHADER_DUMP=<dumps> for disassembly,
EDEN_NX2_FRAME_CAPTURE=<frames> for a preview), then:

    python3 -I tools/nx2/shader_lab.py --diag-dir <dir> [--dump-dir <dumps>] [--frames <frames>]

and open http://127.0.0.1:8765. The emulator writes <dir>/diag-state.json once a second and re-reads
<dir>/ablate.txt and <dir>/inspect.txt when they change; this tool only edits those files (and
labels.json). Nothing here touches the game's own files. Programs are identified by their dump key,
a hash of the code, which stays the same between runs (addresses do not).
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

KEY = re.compile(r"^[0-9a-f]{16}$")
HERE = os.path.dirname(os.path.abspath(__file__))


class Lab:
    def __init__(self, diag_dir, dump_dir, frames_dir):
        self.dir = diag_dir
        self.dump_dir = dump_dir
        self.frames_dir = frames_dir
        self.lock = threading.Lock()
        self.dumps = {}
        self.disassembly = {}

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
        """The newest frame capture as PNG bytes (needs ImageMagick), or None."""
        if not self.frames_dir:
            return None
        try:
            names = sorted(name for name in os.listdir(self.frames_dir) if name.endswith(".tga"))
        except OSError:
            return None
        if not names:
            return None
        source = os.path.join(self.frames_dir, names[-1])
        target = self.path(os.path.join("preview", hashlib.sha1(source.encode()).hexdigest()[:8] + ".png"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        if not os.path.exists(target):
            try:
                subprocess.run(["magick", source, "-resize", "960x", target], check=True, timeout=30)
            except (OSError, subprocess.SubprocessError):
                return None
        with open(target, "rb") as file:
            return file.read()


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
        elif self.path == "/api/capture":
            open(self.lab.path("draw-list"), "a").close()
        else:
            return self.send("not found", "text/plain", 404)
        self.send("{}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--diag-dir", required=True, help="the emulator's EDEN_NX2_DIAG_DIR")
    parser.add_argument("--dump-dir", help="the emulator's EDEN_NX2_SHADER_DUMP (for disassembly)")
    parser.add_argument("--frames", help="the emulator's EDEN_NX2_FRAME_CAPTURE (for the preview)")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    os.makedirs(args.diag_dir, exist_ok=True)
    Handler.lab = Lab(args.diag_dir, args.dump_dir, args.frames)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"Shader lab on http://127.0.0.1:{args.port} (diag dir {args.diag_dir})")
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
</style></head><body>
<header><b>Shader lab</b><span id="meta" class="mut"></span><span class="sp"></span>
<button id="cap" title="Write the next frame's draw list">Capture frame</button></header>
<main>
<div id="left"><div id="tools">
 <span id="kinds"></span>
 <input id="q" placeholder="filter key / address / label" size="22">
 <label><input type="checkbox" id="act" checked> active</label>
 <label><input type="checkbox" id="hon"> hidden only</label>
 <label><input type="checkbox" id="ins"> inspected only</label>
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
const post=(u,b)=>fetch(u,{method:"POST",body:JSON.stringify(b)}).then(()=>refresh());
const esc=s=>String(s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;"}[c]));
const num=n=>n>=1e6?(n/1e6).toFixed(1)+"M":n>=1e4?(n/1e3).toFixed(0)+"k":n;
const isHid=p=>S.ablated.includes(p.key);
const isIns=p=>S.inspected.includes(p.key);
function buildKinds(){$("#kinds").innerHTML=Object.entries(KIND).map(([k,n])=>`<button data-k="${k}" class="${kinds.has(k)?"on":""}">${n}</button>`).join(" ")}
$("#kinds").onclick=e=>{const k=e.target.dataset.k;if(!k)return;kinds.has(k)?kinds.delete(k):kinds.add(k);buildKinds();render()};
async function refresh(){try{S=await (await fetch("/api/state")).json()}catch(e){return}
 $("#meta").textContent=`frame ${S.frame} · ${S.programs.length} programs · ${S.ablated.length} hidden · ${S.dumps} dumps`;render();if(sel&&document.activeElement.id!=="lb")drawDetail()}
function rows(){const q=$("#q").value.toLowerCase();
 return S.programs.filter(p=>kinds.has(p.kind)&&(!$("#act").checked||S.frame-p.last_frame<180)
  &&(!$("#hon").checked||isHid(p))&&(!$("#ins").checked||isIns(p))
  &&(!q||(p.key+" "+p.addr+" "+(S.labels[p.key]||"")).toLowerCase().includes(q)))
  .sort((a,b)=>{const x=a[sortBy],y=b[sortBy];return (x>y?1:x<y?-1:0)*sortDir})}
function render(){shown=rows();const cols=[["kind","l"],["key","l"],["addr","l"],["per_s",""],["draws",""],["instances",""],["vertices",""],["rt",""]];
 $("#list").innerHTML="<table><tr><th class=l></th>"+cols.map(([c,a])=>`<th class="${a}" data-c="${c}">${c==="per_s"?"/s":c}${c===sortBy?(sortDir>0?" ▲":" ▼"):""}</th>`).join("")+"<th class=l>label</th></tr>"+
 shown.slice(0,600).map(p=>`<tr class="row ${sel===p.key+p.kind?"sel":""}" data-id="${p.key}${p.kind}"><td class=l><input type=checkbox data-h="${p.key}" ${isHid(p)?"":"checked"} title="drawn / hidden"></td><td class="l">${KIND[p.kind][0]}</td><td class="l mono">${p.key.slice(0,10)}</td><td class="l mono mut">${p.addr}</td><td>${p.per_s}</td><td>${num(p.draws)}</td><td>${num(p.instances)}</td><td>${num(p.vertices)}</td><td class=mut>${p.rt}</td><td class="l ${isHid(p)?"hid":""}">${isHid(p)?"hidden ":""}${isIns(p)?"◉ ":""}${esc(S.labels[p.key]||"")}</td></tr>`).join("")+"</table>"}
$("#list").onclick=e=>{const h=e.target.dataset.h;if(h){post("/api/ablate",{key:h,on:!e.target.checked});return}
 const th=e.target.closest("th");if(th&&th.dataset.c){const c=th.dataset.c;sortDir=c===sortBy?-sortDir:-1;sortBy=c;render();return}
 const tr=e.target.closest("tr.row");if(tr){sel=tr.dataset.id;render();drawDetail()}};
for(const id of ["#q","#act","#hon","#ins"])$(id).oninput=render;
$("#hall").onclick=()=>post("/api/ablate",{keys:shown.filter(p=>p.key!=="0".repeat(16)).map(p=>p.key),on:true});
$("#hhalf").onclick=()=>post("/api/ablate",{keys:shown.slice(0,Math.ceil(shown.length/2)).filter(p=>p.key!=="0".repeat(16)).map(p=>p.key),on:true});
$("#uall").onclick=()=>post("/api/ablate",{keys:S.ablated,on:false});
$("#cap").onclick=()=>post("/api/capture",{});
function cur(){return S.programs.find(p=>p.key+p.kind===sel)}
async function drawDetail(){const p=cur();if(!p){$("#detail").textContent="Not seen yet.";return}
 const label=S.labels[p.key]||"";
 let body="";
 if(tab==="over"){body=`<table>${Object.entries(p.outcomes).map(([k,v])=>`<tr><td class=l>${k}</td><td>${num(v)}</td></tr>`).join("")}</table>
  <p class=mut>last seen frame ${p.last_frame} (now ${S.frame}); last target ${p.rt}</p>`}
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
 <button id="bi" class="${isIns(p)?"on":""}">${isIns(p)?"Inspecting":"Inspect"}</button>
 <input id="lb" value="${esc(label)}" placeholder="label (water, grass…)" size="22"></p>
 ${p.kind!=="c"?`<img id="fr" src="/api/frame?${Date.now()>>12}" alt="" onerror="this.remove()">`:""}
 <div class="tabs">${[["over","Overview"],["uni","Uniforms"],["dis","Disassembly"]].map(([k,n])=>`<button data-t="${k}" class="${tab===k?"on":""}">${n}</button>`).join("")}</div>${body}`;
 $("#bh").onclick=()=>post("/api/ablate",{key:p.key,on:!isHid(p)});
 $("#bi").onclick=()=>post("/api/inspect",{key:p.key,on:!isIns(p)});
 $("#lb").onchange=e=>post("/api/label",{key:p.key,label:e.target.value});
 for(const b of document.querySelectorAll("[data-t]"))b.onclick=()=>{tab=b.dataset.t;drawDetail()}}
function f32(i){const b=new DataView(new ArrayBuffer(4));b.setUint32(0,i>>>0);const v=b.getFloat32(0);return Number.isFinite(v)&&Math.abs(v)<1e9?+v.toPrecision(6):"-"}
buildKinds();refresh();setInterval(refresh,1000);
</script></body></html>
"""

if __name__ == "__main__":
    main()
