#!/usr/bin/env python3
"""Builds index.html, a contact sheet for an EDEN_NX2_FRAME_DUMP directory.

Usage: py -I tools/nx2/frame_dump_index.py <dump dir>
"""
import html
import json
import sys
from pathlib import Path


def badges(entry):
    out = []
    if entry.get("skipped"):
        out.append(("skip", "skipped: " + entry["skipped"]))
    if any(entry.get("nan") or []) or any(entry.get("inf") or []):
        out.append(("bad", "NaN/Inf (magenta)"))
    lo, hi = entry.get("min"), entry.get("max")
    if lo and hi and lo == hi:
        out.append(("flat", "constant " + ",".join(f"{v:g}" if v is not None else "-" for v in lo)))
    if hi and any(v is not None and v > 1.0 for v in hi[:3]):
        out.append(("hdr", "max > 1"))
    return out


def fmt_list(values):
    if not values:
        return "-"
    return " ".join("-" if v is None else f"{v:.4g}" if isinstance(v, float) else str(v) for v in values)


def card(entry):
    kind = entry.get("kind", "?")
    if kind == "pass":
        title = f"pass {entry['pass']} rt{entry['slot']} · draws {entry['first_draw']}+{entry['draw_count']}"
    elif kind == "dispatch":
        title = f"dispatch {entry['dispatch']} img{entry['slot']} · after draw {entry['after_draws']}"
    elif kind == "sampled" and "dispatch" in entry:
        title = f"dispatch {entry['dispatch']} samples tic {entry['tic']} · after draw {entry['after_draws']}"
    elif kind == "sampled":
        title = f"pass {entry['pass']} samples tic {entry['tic']} (stage {entry['stage']}) · draw {entry['draw']}"
    else:
        title = f"present · after draw {entry.get('after_draws', '?')}"
    png = entry.get("png")
    image = (f'<a href="{html.escape(png)}"><img loading="lazy" src="{html.escape(png)}"></a>'
             if png else '<div class="noimg">no preview</div>')
    rows = [
        ("format", f"{entry.get('format', '?')} {entry.get('width', '?')}x{entry.get('height', '?')}"
                   + (f"x{entry['depth']}" if entry.get("depth", 1) > 1 else "")
                   + f" L{entry.get('level', 0)}/{entry.get('layer', 0)}"
                   + (" rescaled" if entry.get("rescaled") else "")),
        ("gpu", entry.get("gpu_addr", "-")),
        ("shaders", " ".join(entry.get("shaders", [])) or "-"),
        ("keys", " ".join(key or "-" for key in entry.get("keys", [])) or "-"),
        ("min", fmt_list(entry.get("min"))),
        ("max", fmt_list(entry.get("max"))),
        ("mean", fmt_list(entry.get("mean"))),
    ]
    if any(entry.get("nan") or []) or any(entry.get("inf") or []):
        rows.append(("nan/inf", f"{fmt_list(entry.get('nan'))} / {fmt_list(entry.get('inf'))}"))
    table = "".join(f"<tr><th>{k}</th><td>{html.escape(str(v))}</td></tr>" for k, v in rows)
    tags = "".join(f'<span class="b {c}">{html.escape(t)}</span>' for c, t in badges(entry))
    return (f'<div class="card {kind}" id="{html.escape(entry["name"])}"><div class="t">{html.escape(title)}</div>'
            f"{tags}{image}<table>{table}</table><div class=\"n\">{html.escape(entry['name'])}</div></div>")


STYLE = """
body{font:12px system-ui,sans-serif;background:#1b1b1f;color:#ddd;margin:16px}
h1{font-size:16px}.grid{display:flex;flex-wrap:wrap;gap:10px}
.card{background:#26262c;border-left:4px solid #555;padding:6px;width:300px}
.card.pass{border-color:#4a8}.card.sampled{border-color:#86c}.card.dispatch{border-color:#a84}.card.present{border-color:#48c}
.t{font-weight:600;margin-bottom:4px}.n{color:#888;word-break:break-all;margin-top:4px}
img{max-width:300px;max-height:220px;display:block;background:repeating-conic-gradient(#333 0 25%,#2a2a2a 0 50%) 0 0/16px 16px}
.noimg{height:60px;display:flex;align-items:center;justify-content:center;color:#777;border:1px dashed #444}
table{border-collapse:collapse;width:100%}th{text-align:left;color:#999;padding-right:6px;vertical-align:top}
td{word-break:break-all}.b{display:inline-block;padding:1px 5px;margin:0 4px 4px 0;border-radius:3px;background:#444}
.b.bad{background:#a0a}.b.flat{background:#666;color:#fff}.b.hdr{background:#a60}.b.skip{background:#733}
"""


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = Path(sys.argv[1])
    entries, header, summary = [], {}, {}
    for line in (root / "manifest.jsonl").read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        entry = json.loads(line)
        if entry.get("kind") == "frame":
            header = entry
        elif entry.get("kind") == "summary":
            summary = entry
        elif entry.get("kind") in ("buffer", "launch"):
            continue
        else:
            entries.append(entry)
    info = (f"frame {header.get('frame', '?')} at {header.get('seconds', '?')} s · "
            f"{summary.get('images', len(entries))} images · {summary.get('passes', '?')} passes · "
            f"{summary.get('draws', '?')} draws · {summary.get('dispatches', '?')} dispatches · "
            f"{summary.get('bytes', 0) / 2**20:.1f} MiB"
            + (" · TRUNCATED at the size cap" if summary.get("truncated") else "")
            + ("" if summary else " · no summary line: the capture did not finish"))
    page = (f"<!doctype html><meta charset=utf-8><title>Frame dump</title><style>{STYLE}</style>"
            f"<h1>Frame dump: {html.escape(info)}</h1>"
            "<p>Float targets are sRGB-encoded for display and clamped to [0,1]; magenta marks NaN/Inf pixels; "
            "alpha is not shown. Exact values are in the .raw.zst files (tightly packed rows).</p>"
            f'<div class="grid">{"".join(card(e) for e in entries)}</div>')
    out = root / "index.html"
    out.write_text(page, encoding="utf-8")
    print(f"{out}: {len(entries)} entries")


if __name__ == "__main__":
    main()
