# MCP Server

The built-in [Model Context Protocol](https://modelcontextprotocol.io) server lets AI agents and scripts
drive and inspect the emulator. User-facing documentation: [User Handbook - MCP](./user/MCP.md).
Tool list: [`tools/mcp/README.md`](../tools/mcp/README.md).

It is compiled in with the `ENABLE_MCP_SERVER` CMake option (ON) and stays off at runtime until the
user enables it (`enable_mcp_server`, `--mcp`).

## Layout

```
src/mcp/
  protocol.*        Streamable HTTP rules, sessions, version negotiation, JSON-RPC dispatch.
                    Transport independent and unit tested (src/tests/mcp/).
  server.*          cpp-httplib binding on 127.0.0.1, extra endpoints (dashboard, SSE,
                    /events/wait), resources and prompts, log sink, discovery file.
  tool.*, tool_registry.*   Tool definitions, JSON-schema argument validation.
  timeline.*        Sequence-numbered ring of events (logs, input, IPC, SVC, crashes,
                    unimplemented commands, watches) read with cursors.
  control.*         Control lease: one session at a time may change emulator state.
  activity.h        What the server is doing, for the status bar and dashboard.
  host.h            Mcp::Host, implemented by each frontend.
  tools/*.cpp       One file per area; RegisterAllTools() in core_tools.cpp lists them.
src/yuzu/mcp_host_qt.*       Qt host (marshals to the GUI thread).
src/yuzu_cmd/mcp_host_cli.*  eden-cli host (actions drained on the SDL main thread).
```

## Frontend host

The server never touches widgets. Anything that belongs to the frontend (game list, boot, pause,
stop, screenshots, TAS control, docked mode, applying settings) goes through `Mcp::Host`. The Qt host
runs these on the GUI thread with blocking queued calls, so host methods must not be called from
the GUI thread itself. `Server::CallTool` exists for the frontend's own UI (the Routes menu) and only
calls tools that do not marshal back to the GUI thread.

## Adding a tool

Add it to the matching `tools/*.cpp` (or a new file registered in `RegisterAllTools`):

```cpp
registry.Add(Tool{
    .name = "read_thing",
    .title = "Read the thing",
    .description = "What it returns and when to use it.",
    .input_schema = {{"type", "object"},
                     {"properties", {{"id", {{"type", "integer"}, {"minimum", 0}}}}},
                     {"required", {"id"}},
                     {"additionalProperties", false}},
    .annotations = {.read_only = true},
    .requires_game = true,     // refused when nothing runs
    .requires_write = false,   // refused unless Allow Writes is on
    .requires_control = false, // needs the control lease
    .handler = ReadThing,
});
```

Domain failures return `ToolResult::Error(...)` so the agent sees them; protocol failures are raised
by the server. `src/tests/mcp/protocol.cpp` checks every registered tool's definition, so keep the
schema an object with `additionalProperties: false` and give writes the control lease too.

## Core hooks

The server observes the emulator through a few optional hooks. Unset, each costs one relaxed atomic
load on its path:

- `Core::Trace::g_ipc_hook`, `g_svc_hook`, `g_crash_hook` (`core/debugger/trace_hooks.h`): IPC
  dispatch, SVC calls, `svcBreak` and the fatal service. SVC arguments are only copied while a hook
  is set.
- `Service::g_unimplemented_hook` (`core/hle/service/unimplemented_registry.h`): the first call of each
  unimplemented command, with the caller's backtrace (only symbolized while the hook is set).
- `Common::Log::SetExternalSink`: log lines into the timeline (key-like hex is masked).
- `VideoCommon::Nx2Profile`: the GPU-thread profiler can be switched on at runtime and publishes a
  per-thread snapshot per report window.

The server installs these on start and removes them on stop.

## Threading and cost

HTTP requests are served by a small thread pool. Long waits (`wait_for`, `act`, routes,
`/events/wait`) hold one worker each. Background work (memory watches, route runs, recording) runs on
its own thread and stops with the server. Screen checks request a frame copy from the renderer, so
polling stays at about 4 per second.

## Testing

`tests "[mcp]"` covers the protocol, sessions, validation, the timeline, the control lease and every
tool definition. For end-to-end checks, enable the server and use the MCP Inspector
(`npx @modelcontextprotocol/inspector`, transport Streamable HTTP) or `tools/mcp/scripts/mcp.py`.
