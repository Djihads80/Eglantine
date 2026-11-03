# User Handbook - MCP Server (AI agents)

Eglantine has a built-in [Model Context Protocol](https://modelcontextprotocol.io) (MCP) server. MCP is
the standard way AI agents (Claude Code, Claude Desktop, Cursor, the MCP Inspector, scripts...) use
external tools. With the server on, an agent can watch and drive the emulator: boot games, press
buttons, take screenshots, read logs and memory, trace service calls, profile the GPU threads, and
replay recorded routes. It is meant for testing and debugging: reproducing a bug, checking a fix,
finding out why a game stops booting, or measuring performance before and after a change.

The server is **off by default** and only listens on `127.0.0.1` (this PC).

## Turning it on

- **Tools > MCP Server > Enabled** (remembered), or
- start the emulator with `--mcp` (this run only). See [Command Line](./CommandLine.md).

Agents can only *observe* until you also turn on **Tools > MCP Server > Allow Writes** (or start
with `--mcp-allow-writes`). Writes cover anything that changes the emulator: input, memory writes,
settings, booting and stopping games, running routes.

While the server runs, the status bar shows `MCP :6544`, the agent in control, the last tool it
called, and any route or recording in progress (red while recording). Hover it for details.

## Connecting an agent

The endpoint is `http://127.0.0.1:6544/mcp` (Streamable HTTP). **Tools > MCP Server** has actions that
copy the URL or the Claude Code command:

```sh
claude mcp add --transport http eglantine http://127.0.0.1:6544/mcp
```

Other clients: add an HTTP MCP server with that URL. Clients that only speak stdio can use a bridge
such as `npx mcp-remote http://127.0.0.1:6544/mcp`.

Several agents can connect at once. Only one at a time may change things (the *control lease*, given
automatically to the first agent that acts and released after two idle minutes); the others can
still observe.

## Watching what an agent does

**Tools > MCP Server > Open Dashboard** opens `http://127.0.0.1:6544/` in your browser: the live
screen, status, which agent is in control, the last tool used, route progress, and a live event log
you can filter. It is read-only.

## Routes: recording what you play

A *route* is a saved recipe for a game, such as "from a cold boot to standing in the level", that an
agent (or you) can replay later. Routes wait for the right screen before each input, so they keep
working when loading takes longer or shorter.

**Tools > MCP Server > Routes** (with a game running):

- **Record New Route...**: name it, then play normally. Pause for a moment on each screen before
  pressing a button: input after a settled screen becomes a *checkpoint* ("wait for this screen,
  then press"), input while the picture is moving is replayed by timing.
- **Stop Recording** saves it and shows the steps.
- Each saved route has **Run** and **Delete**. **Stop Route** stops one that is running.

Tips: menus, loading screens and cutscene skips replay very reliably. Long walks are replayed by
timing, so they can drift when the frame rate varies; keep them short. If the screen used for a
checkpoint changes (a graphics fix or a different version), record the route again.

Routes, screen labels and memory probes are stored per game under
`<user folder>/config/mcp/` (see [Storage](./Storage.md)).

## Safety

- Only programs on this PC can connect. Web pages from other sites are refused.
- Optional `mcp_token` setting: when set, clients must send it as a bearer token.
- Keys, console serials and the MCP's own security settings are never exposed to agents, and key
  material is masked in the log lines they read.
- With Allow Writes off (the default), agents cannot change anything.

## What agents can do

The full list of tools, with what each one does, is in
[`tools/mcp/README.md`](../../tools/mcp/README.md). In short:

| Area | Examples |
|---|---|
| Games | list, boot, pause, resume, stop, docked/handheld, game info |
| Seeing | screenshots, frame bursts, "which screen is this" from labelled screens |
| Input | buttons, sticks, touch, timed sequences, frame-exact TAS scripts, recording |
| Events | a timeline of logs, crashes and unimplemented service commands (with the caller's backtrace), waits for events, summaries, comparing two runs |
| Debugging | guest memory read/search/watch/scan, threads and backtraces, IPC and SVC tracing, settings, debug knobs |
| Performance | frame pacing, GPU-thread profile, host CPU per emulator thread, visual baselines |

`eden-cli` supports the same tools for unattended runs: `eden-cli --mcp -g <game>`.
