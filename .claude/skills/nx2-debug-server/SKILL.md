---
name: nx2-debug-server
description: "Use when asking a running NX2 game questions without relaunching: screenshots, live shader rejects, frame captures, guest/GPU memory reads, injected input, log tail. Also covers the shader catalogue (tools/nx2/catalog) workflow."
version: 1.0.0
author: kraibse
license: MIT
---

# NX2 debug server, MCP adapter and shader catalogue

Full reference: `docs/nx2/debug-mcp.md`, `tools/nx2/catalog/README.md`.

## Rules

- Only talk to an emulator the user launched with `EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545` (or one you started via `lab_ctl.py launch --max-seconds`). Never start or stop the user's emulator.
- Try a live experiment before asking for a new build or run.
- Restore every hash you reject, every injected input and every memory write before finishing.
- `write_memory` needs `EDEN_NX2_DEBUG_WRITE=1` on the emulator; leave it off unless needed.

## Access

Two MCP servers are registered in `.mcp.json` and enabled in `.claude/settings.json`; they complement each other:

- `eglantine` (HTTP, 127.0.0.1:6544, built into the emulator, PR #421): emulator-wide. Boot/pause/stop games, event timeline (logs, crashes, unimplemented service commands), IPC/SVC trace, memory watch, GDB breakpoints, GPU profile, routes, A/B tests. Needs the server enabled (Tools > MCP Server or `--mcp`); input and memory writes need Allow Writes. Client: `tools/mcp/scripts/mcp.py`.
- `nx2-debug` (stdio adapter to the loopback debug server on 6545): rendering and shaders. `capture_frame`, live `reject_shaders` by key/hash, `read_gpu_memory`, frame-exact `input_sequence`. Needs `EDEN_NX2_DEBUG_SERVER`.

Use `eglantine` to start the game and watch its behaviour, `nx2-debug` to bisect shaders and inspect frames. Tool names overlap (status, screenshot, read_memory) but are namespaced per server. Prefer one input source at a time.

`nx2-debug` runs `python3 -I tools/nx2/debug_mcp.py`, which connects to `EDEN_NX2_DEBUG_SERVER` (default `127.0.0.1:6545`). It reconnects after a game restart. If the MCP tools are unavailable, use one-shot calls:

```sh
python3 -I tools/nx2/debug_mcp.py --call '{"method":"status"}'
python3 -I tools/nx2/debug_mcp.py --call '{"method":"screenshot","params":{"height":1080}}' --out shot.png
python3 -I tools/nx2/debug_mcp.py --call '{"method":"reject_shaders","params":{"hashes":["<hex16>"]}}'
python3 -I tools/nx2/debug_mcp.py --call '{"method":"reject_shaders","params":{"hashes":["<hex16>"],"reject":false}}'  # restore
python3 -I tools/nx2/debug_mcp.py --call '{"method":"reject_shaders"}'  # no hashes: list rejected
python3 -I tools/nx2/debug_mcp.py --call '{"method":"capture_frame","params":{"buffers":true}}'
```

Hashes and keys must be all 16 hex digits.

## Tools

| Tool | Use |
|---|---|
| `status` | title, program id, fps, paused, rejected hashes |
| `screenshot` | PNG of next presented frame |
| `log_tail`, `log_filter` | new log lines; change filter live |
| `capture_frame` | frame dump now (`sampled`, `buffers` options); `index` builds contact sheet |
| `capture_image` | one PNG of a capture made this session |
| `reject_shaders` | reject/restore programs by native key or shader hash |
| `read_memory`, `search_memory`, `read_gpu_memory` | guest/GPU memory (<= 64 KiB per read) |
| `input`, `input_sequence` | buttons/sticks for exact numbers of presented frames |
| `sample` | profiler (Windows only) |

Limits: `capture_frame`, `reject_shaders`, `read_gpu_memory` are Vulkan only; fps and input follow Switch 2 presents; one request at a time; no pause or frame step.

## Bisecting a visual bug

1. `screenshot` baseline. 2. `reject_shaders` half the candidates (keys from `diag-state.json` or the catalogue). 3. `screenshot`, compare. 4. Halve again. 5. Restore all. 6. Record the finding in the catalogue.

## Keys vs hashes

- `key`: native program key, hash of the game's code, same in every build. Shader dump names, lab, `ablate.txt`.
- `hash`: shader hash of the lowered code; changes when that program's lowering changes. Frame dump `shaders`, `reject_shaders`.
- Both work in `reject_shaders` and `EDEN_NX2_REJECT_HASH`.

## Shader catalogue

`tools/nx2/catalog/<program id>.json` (one per game, Ocarina of Time = `0400486024DEC000`). Entries: key, hash, stage, object, group, how, notes, source, updated. Names, stages and short notes only; no shader code, textures or dumps.

The shader lab keeps a local copy (`EDEN_NX2_CATALOG_DIR`, default `~/.local/share/eglantine/nx2-catalog`) and merges the repo copy at start.

```sh
python3 -I tools/nx2/catalog.py find <key or hash>
python3 -I tools/nx2/catalog.py show --title <id>
python3 -I tools/nx2/catalog.py add --title <id> --key <k> [--hash <h>] --object "..." --how "..."
python3 -I tools/nx2/catalog.py status   # before every commit; exit 1 = local has more
python3 -I tools/nx2/catalog.py merge    # then commit tools/nx2/catalog/*.json on its own:
                                         # "tools/nx2: catalogue: <game>: <n> programs"
```

`merge` only adds/updates; never delete entries from the repo copy. Look a program up with `find` before investigating it from scratch.
