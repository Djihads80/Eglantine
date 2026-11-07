# Built-in MCP server

Eglantine can expose a [Model Context Protocol](https://modelcontextprotocol.io) server so AI
agents can inspect and drive the running emulator.

## Enable

The server is **off by default**. Turn it on in one of these ways:

- **Tools > MCP Server > Enabled** (remembered as the `enable_mcp_server` setting), or
- start the emulator with `--mcp` (this run only; `--mcp-port <port>` implies `--mcp`).

It listens on `http://127.0.0.1:6544/mcp` (Streamable HTTP, loopback only, so only programs on
this PC can reach it). The **Tools > MCP Server** menu also has *Allow Writes* (lets agents change
emulator state), *Routes*, *Open Dashboard* and actions that copy the URL or the `claude mcp add`
command to the clipboard. A status-bar label shows whether it is running and what agents are doing.

Settings: `enable_mcp_server` (default off), `mcp_port` (6544), `mcp_allow_writes` (off),
`mcp_token` (optional bearer token). Command line: `--mcp`, `--mcp-port <port>`,
`--mcp-allow-writes`.

Builds include the server unless configured with `-DENABLE_MCP_SERVER=OFF`.

## Connect

Claude Code:

```sh
claude mcp add --transport http eglantine http://127.0.0.1:6544/mcp
```

Or a project `.mcp.json`:

```json
{ "mcpServers": { "eglantine": { "type": "http", "url": "http://127.0.0.1:6544/mcp" } } }
```

Stdio-only clients can use a bridge such as `npx mcp-remote http://127.0.0.1:6544/mcp`.
Test with `npx @modelcontextprotocol/inspector` (transport: Streamable HTTP).

## Tools

W = needs *Allow Writes*; L = needs the control lease (granted automatically when no other
agent holds it, see `acquire_control`).

| Tool | What it does |
|---|---|
| `status` | Running/paused, program id, title, build id, guest frame, perf, version |
| `timeline` | Cursor-based read of everything recorded (log lines, state, marks...) |
| `wait_for` | Block until a matching timeline event happens |
| `mark` | Insert a labelled marker into the timeline |
| `set_log_filter` (L) | Change log classes/levels at runtime |
| `act` | Input (W, L) + wait (event / guest frames / ms) + capture (screenshot, timeline, controller, perf) in one call |
| `list_games` | Games in the game list |
| `boot_game`, `pause`, `resume`, `stop`, `quit_emulator` (L) | Lifecycle |
| `screenshot` | PNG of the presented frame |
| `press_buttons`, `set_stick`, `input_sequence`, `release_all` (W, L) | Virtual gamepad input (wall-clock) |
| `get_controller_state` | Buttons/sticks as the game sees them |
| `tas_status`, `tas_read_script` | TAS engine state; read a script or the last recording |
| `tas_write_script`, `tas_play`, `tas_stop`, `tas_record_start`, `tas_record_stop`, `tas_set_options` (W, L) | Frame-locked TAS scripts (write as steps or raw TAS-nx, play with screenshots at frames, record a human) |
| `screen_label`, `screen_state`, `screen_labels` | Name screens by fingerprint per game ("title", "loading", "gameplay"), then ask which screen is showing without an image |
| `probe_save`, `probe_read` | Named game variables (`Module+0xOFF`, pointer chains `->0x10`, value meanings) |
| `route_save`, `route_list`, `route_run` (W, L) | Per-game recipes: input + waits for a screen / end of a screen / probe value, repeat-until; e.g. `to_gameplay` |
| `route_status`, `route_stop` (L) | Progress of a running (or background) route; stop a background route |
| `route_record_start`, `route_record_stop` | Record what is played into a route (checkpoints at settled screens, timed input otherwise) |
| `screenshot_raw` | Capture-layer frame: 1280x720, no post-processing, works hidden/paused |
| `game_info`, `firmware_status` | Running game metadata and icon; installed firmware |
| `process_info`, `memory_map`, `list_threads`, `backtrace` | Process layout and modules, mapped regions, guest threads with CPU time, symbolized backtraces |
| `read_memory`, `search_memory`, `write_memory` (W, L) | Guest memory (typed or hex; pattern search with `??`) |
| `watch_add`, `watch_list`, `watch_remove` | Values sampled every frame; changes become `watch` timeline events |
| `scan_start`, `scan_next`, `scan_results` | Cheat-Engine-style search for an unknown address |
| `ipc_trace`, `ipc_summary`, `svc_trace`, `list_services` | Runtime IPC/SVC tracing into the timeline, call summary, registered services |
| `debugger_status`, `debugger_attach`, `debugger_detach` | The emulator's GDB stub as the MCP's debugger (the stub must be on when the game boots; `boot_game` attaches automatically then) |
| `breakpoint_add` (W, L), `breakpoint_list`, `breakpoint_remove` (L) | Exec breakpoints and read/write/access watchpoints by address or `Module+0xoffset`; hits are `break` timeline events with registers and a backtrace (mode `log`), or leave the game paused (mode `stop`) |
| `debugger_pause`, `debugger_step`, `debugger_continue` (L) | Pause through the debugger, single-step, resume |
| `dump_set` (W, L), `dump_list`, `dump_read` | Shader (.ash), SPIR-V and macro dumps: turn on, list what was written, read part of a file |
| `report_list`, `report_read` | Crash, svcBreak, unimplemented-function and other reports (with `reporting_services` on) |
| `fs_access_log` | The FS access log a game writes (with `enable_fs_access_log` on before boot) |
| `ab_test` (W, L), `ab_test_status`, `ab_test_stop` (L) | Measure 2-6 values of a setting with a route or perf sample each (reboots when the setting needs it) and restore it |
| `unimplemented_commands` | Unimplemented service commands with counts, request words and the first caller's backtrace (also posted live as `unimpl` events) |
| `gpu_profile` | NX2 GPU-thread profiler toggled live, per-thread zones and counters as JSON |
| `perf_sample` | Frame pacing (fps, p95/p99 frame times, 1% low), a GPU profile of the same window and host CPU per thread, plus a baseline check or screenshot |
| `baseline_save`, `baseline_check` | Save a known-good frame per game; score the current frame against it |
| `summarize_since`, `timeline_export`, `compare_runs` | Clustered digest; JSONL export under `<log dir>/mcp`; diff two runs |
| `settings_list`, `settings_get`, `settings_set` (W, L) | Settings by name (credentials, serials and MCP security settings are hidden) |
| `debug_knobs`, `set_determinism` (W, L), `env_switches` | A/B knobs, fixed RNG/RTC/language/region, active `EDEN_*` variables |
| `tap`, `swipe` (W, L) | Touch screen input (0..1 coordinates) |
| `set_docked` (W, L) | Docked/handheld live, with the frontend's controller swap |
| `frame_advance` (L), `capture_frames` | Run N frames then pause (+ screenshot); a screenshot burst every N frames |
| `control_status`, `acquire_control`, `release_control` | Share one emulator between several agents |

MCP resources: `eglantine://tools/guide`, `eglantine://status`, `eglantine://game/current`
(metadata + saved labels/routes/probes), `eglantine://timeline/summary`. Prompts: `boot_test`,
`investigate_unimplemented`, `perf_check`, `compare_change`. While running, the server writes
`<user dir>/mcp-server.json` (url, port, pid; never the token). Log events redact key-like hex.

Crashes (`svcBreak`, fatal) are posted as `crash` timeline events with the thread's backtrace.

## Other HTTP endpoints (same port, same Host/Origin/token checks)

| Endpoint | Use |
|---|---|
| `GET /` | Read-only dashboard: live screen, status, screen label, control holder, live event log |
| `GET /events/wait?kinds=crash,unimpl&timeout_s=600` | Blocks until a matching event, then returns it: run it as a background `curl` to be woken by a crash |
| `GET /stream/timeline?min_level=info` | The timeline as server-sent events |
| `GET /mcp` + `Accept: text/event-stream` + session | MCP server-to-client stream: `notifications/message` for crashes, new unimplemented commands, warning+ logs |
| `GET /status.json`, `GET /screen.png?w=640` | Data behind the dashboard |

With `mcp_token` set, pass `Authorization: Bearer <token>` (or `?token=` in a browser).

## eden-cli

`eden-cli --mcp [--mcp-port N] [--mcp-allow-writes] -g <game>` serves the same tools for
unattended runs (no game list: the game is the one on the command line; `stop`/`quit_emulator`
end the process). eden-cli starts the server only with `--mcp`.

## Code

`src/mcp/`: transport-independent protocol (`protocol.*`), tool registry and argument
validation (`tool_registry.*`), event timeline (`timeline.*`), server/HTTP binding and extra
endpoints (`server.*`, `dashboard.h`), tools (`tools/`, shared pieces in `tool_helpers.h`).
Frontends implement `Mcp::Host`: `src/yuzu/mcp_host_qt.*` (Qt), `src/yuzu_cmd/mcp_host_cli.*`
(eden-cli). Core observers the MCP plugs into: `core/debugger/trace_hooks.h` (IPC, SVC,
crashes), `Service::g_unimplemented_hook`, `VideoCommon::Nx2Profile` snapshots.
Per-game data: `<config>/mcp/{screens,probes,routes}/<program id>.json`.
Unit tests: `src/tests/mcp/`. Minimal command-line client: `tools/mcp/scripts/mcp.py <tool> [json-args]`
(set `EGLANTINE_MCP_TOKEN` when a token is configured, and `EGLANTINE_MCP_SID_FILE` per agent so
each one gets its own session and control lease).
