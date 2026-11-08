# Debug server, frame dump and MCP adapter

Most `EDEN_NX2_*` switches are read once at startup, so every new question about a running game
used to cost a relaunch, and often a rebuild. The debug server is an opt-in loopback server inside
the emulator that answers those questions while the game runs, and `tools/nx2/debug_mcp.py` is an
MCP adapter (and a one-shot command line client) for it. Ported from Lichii (zortos293/Lichii #111
and the debug parts of #184).

Everything here is off unless its variable is set:

| Variable | Effect |
|---|---|
| `EDEN_NX2_DEBUG_SERVER=<port>` or `<host>:<port>` | Starts the debug server once a game has loaded. Loopback hosts only. |
| `EDEN_NX2_DEBUG_DIR=<dir>` | Where the server writes captures, screenshots and samples. Default `<log dir>/debug-server`. |
| `EDEN_NX2_DEBUG_WRITE=1` | Lets the test-only `write_memory` tool change guest memory. Without it the tool refuses. |
| `EDEN_NX2_FRAME_DUMP=<dir>` | Reads back one whole frame (see "Frame dump"). |
| `EDEN_NX2_FRAME_DUMP_AT=<seconds>`, `<frame>f` or `file` | When `EDEN_NX2_FRAME_DUMP` captures (default 60 s after boot; `file`: once `<dir>/capture.now` exists). |
| `EDEN_NX2_FRAME_DUMP_SAMPLED=1` | The frame dump also reads back sampled textures and bound buffers. |
| `EDEN_NX2_REJECT_HASH=<hash>,...` | Rejects native programs by native program key or shader hash, as if the lowering refused them. |

## Running it

1. Start `eglantine.exe` (Qt) or `eden-cli.exe` with `EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545` in
   the environment and boot a game. The log (`%APPDATA%\eglantine\log\eden_log.txt`, or `user\log\`
   next to the exe for portable builds) shows `Debug server listening on 127.0.0.1:6545, dump dir
   ...`. The default dump directory is the `debug-server` folder in that log folder.
2. Run the adapter with native Windows Python (`py`). The MSYS `python` in Git Bash rejects `C:/`
   paths. `--call` runs one request, prints its JSON result and saves any PNG to `--out`:

   ```
   py -I tools/nx2/debug_mcp.py --server 127.0.0.1:6545 --call '{"method":"status"}'
   py -I tools/nx2/debug_mcp.py --call '{"method":"screenshot","params":{"height":1080}}' --out shot.png
   py -I tools/nx2/debug_mcp.py --call '{"method":"reject_shaders","params":{"hashes":["3ddfb1d7621c4755"]}}'
   py -I tools/nx2/debug_mcp.py --call '{"method":"capture_frame","params":{"buffers":true}}'
   py -I tools/nx2/debug_mcp.py --call '{"method":"input_sequence","params":{"steps":[{"right_stick":[1,0],"frames":60},{"frames":30}]}}'
   ```

   `--server` defaults to `EDEN_NX2_DEBUG_SERVER` when that is set, else `127.0.0.1:6545`. The
   request is `{"method": <tool>, "params": {...}}`, the format the socket uses. Adapter-only tools
   work too (`capture_image`, and `index` on `capture_frame`). On failure `--call` prints
   `{"error": ...}` and exits with status 1. Without `--call` the script is a stdio MCP server for
   an MCP client.
3. Hashes and keys must be all 16 hex digits. A shorter one is refused rather than zero-padded,
   because a padded hash matches nothing. Returned paths use forward slashes.

The adapter reconnects after the game restarts. A boot through a guest relaunch stops and starts
the server again on the same port. Restore every rejection, injected input and memory write before
you finish.

## Using it with the shader lab

The shader lab (`tools/nx2/shader_lab.py`, `EDEN_NX2_DIAG_DIR`) and the debug server can run
together:

- `reject_shaders` and the lab's `ablate.txt` both accept native program keys (`key <hex16>` in
  `ablate.txt`). Rejecting drops the program at lowering, so its draws and dispatches fail as an
  unsupported program's do; ablating skips the program's draws while it stays lowered.
- `diag-state.json` lists each program with its `key` (native program key, the shader dump name)
  and `hash` (the shader hash, `""` until the program is lowered), and the running program id as
  `title`. `reject_shaders` takes either value, and the frame dump writes each list of shader
  hashes with the matching keys next to it, so a lab row, a capture entry and a rejection can be
  matched.

## Architecture

```
agent --stdio MCP--> tools/nx2/debug_mcp.py --TCP 127.0.0.1, JSON lines--> emulator
                       (Python, stdlib only)                     |- "DebugServer" thread
                                                                 |    status, screenshot, log, memory, input
                                                                 '- rasterizer thread
                                                                      takes requests at each frame end
```

### In-process server

`src/core/debugger/nx2_debug_server.cpp` is the transport. It listens on a loopback TCP port with
Boost.Asio, as the GDB stub does (`src/core/debugger/debugger.cpp`), and runs one `io_context` on
its own thread named `DebugServer`. Each line a client sends is one request,
`{"id":1,"method":"status","params":{}}`; each answer is one line, `{"id":1,"result":{...}}` or
`{"id":1,"error":"..."}`. The transport knows nothing about the emulator: it maps method names to
handlers, which keeps it unit-testable without a `System`.

`src/core/debugger/nx2_debug_tools.cpp` holds the tools. `Core::System::Impl::Load` starts the
service once a game has loaded and `ShutdownMainProcess` stops it before anything else. The member
is declared last in `System::Impl`, so it is also destroyed first. The tools therefore never see a
half-built or half-destroyed system, and the server works the same under the Qt frontend and
`eden-cli`.

The GDB stub speaks the GDB remote protocol and parks emulation on every stop. It stays the tool
for guest breakpoints (`tools/nx2/gdbrsp.py` drives it), but it has no place for frames, shaders or
logs. The debug server copies its threading model, not its protocol.

### Why JSON lines over TCP, with a separate adapter

- MCP's own transports are stdio, which needs the agent to launch the process (the owner starts
  the emulator), and Streamable HTTP, which needs sessions and server-sent events. Neither fits
  inside the emulator cheaply.
- A JSON-lines socket is about 200 lines of C++ with libraries `core` already links (Boost.Asio,
  nlohmann::json). Anything can drive it: the adapter, a test, or `python -c`.
- The adapter owns everything that is MCP: tool descriptions, input schemas, image content and
  error wording. Changing them needs no emulator rebuild.
- Loopback TCP works the same on Windows and Linux.

### Threading

1. **Server thread.** Parses requests and runs handlers. Handlers that only read thread-safe state
   run here directly: status, log tail, log filter and the guest memory tools (see "Guest and GPU
   memory"). Handlers run one at a time, so a slow request (a capture can take seconds) delays the
   next one. One agent at a time is the expected use.
2. **Rasterizer thread.** Everything that touches the texture cache, pipeline cache, scheduler or
   frame dump must run on the thread that owns them. With the GPU thread split that is the back
   thread, which also runs `RendererVulkan::Composite` and therefore `rasterizer.TickFrame()`.
   Requests for it go through `src/video_core/nx2_debug_control.h`, a small mutex-guarded block
   with one atomic "something is pending" flag. `RasterizerVulkan::TickFrame` checks the flag and
   takes the requests at the end of each presented frame. Between frames no draw is half-recorded
   and no pipeline is in use by the caller, so dropping cache entries there is safe.
3. **Existing renderer hooks.** The screenshot uses `RendererBase::RequestScreenshot`, the path the
   Qt screenshot hotkey uses. It is safe to call from another thread, and the renderer fills the
   buffer at the next present.

The server thread waits for rasterizer-thread work with a timeout (5 s for screenshots and
rejections, 120 s by default for a capture). If the game is paused or loading, no frame is
presented and the tool fails with "no frame was presented within N s (paused or loading?)" instead
of hanging. A withdrawn capture request is removed from the queue. On shutdown the service ends the
session first, which wakes every waiter, then stops the `io_context`, so shutdown waits at most
about 50 ms for a handler.

Cost when unused: if `EDEN_NX2_DEBUG_SERVER` is unset, `MaybeStartService` returns at its first
`getenv`: no thread, no socket, no log ring. The per-frame check is one relaxed atomic load, the
runtime reject check in the lowering is one relaxed atomic load per lowered program, and the log
ring backend does one relaxed load per log line. While no capture runs, the frame dump hooks in
draws and dispatches are a member test, and the pipelines check one relaxed atomic before keeping
sampled textures or written images. When the server is on but idle, its thread sleeps in IOCP or
`epoll`.

## Safety

- **Loopback only.** `ParseEndpoint` accepts `127.0.0.1`, `::1` and `localhost` and refuses every
  other host, `0.0.0.0` included. The server is reachable only from the PC itself.
- **Off by default.** It needs `EDEN_NX2_DEBUG_SERVER`. Release packages are unchanged unless
  someone sets it.
- **Files only under the dump directory.** Clients never send paths; the server names every file
  itself: `capture-<session>-<n>/`, `screenshot-<session>-<n>.png`, `samples-<session>-<n>.txt`.
- **Nothing the server returns comes from game files, keys or firmware.** It returns rendered
  pixels, log lines, frame-dump manifests and shader hashes. The memory tools read only the
  application's guest virtual memory and GPU virtual memory through their page tables: an address
  resolves only through a page the guest has mapped, so they cannot reach host memory, the key area
  or the file system. The adapter's `capture_image` reads only PNGs inside capture directories it
  created in this session, and checks for path escapes.
- **Guest memory is never written** unless the emulator was started with `EDEN_NX2_DEBUG_WRITE=1`
  (see `write_memory`). Each write is logged.
- **No authentication.** Any local process may connect, as with the GDB stub. That is acceptable on
  a single-user PC with the switch off by default. On a shared machine, add a token file in the
  dump directory that the adapter reads.

## Tools

| Tool | Runs on | What it does |
|---|---|---|
| `status` | server | Title, program id, build, renderer, paused, presents, fps, dump dir, log filter, rejected hashes, whether `write_memory` is on |
| `screenshot` | renderer hook | PNG of the next presented frame, as MCP image content |
| `log_tail`, `log_filter` | server | New lines since the last call; change the filter live |
| `capture_frame` | rasterizer | The frame dump of the next frame, now; sampled textures and bound buffers optional |
| `capture_image` | adapter | One PNG of a capture made in this session |
| `reject_shaders` | rasterizer | `EDEN_NX2_REJECT_HASH` at runtime: reject or restore programs live |
| `sample` | server | In-process sampling profiler of named host threads (Windows only) |
| `read_memory`, `search_memory` | server | Read or search the game's guest virtual memory |
| `write_memory` | server | Test only, needs `EDEN_NX2_DEBUG_WRITE=1`: write guest memory |
| `read_gpu_memory` | rasterizer | Read GPU virtual memory as the GPU sees it |
| `input`, `input_sequence` | server | Hold buttons and sticks for exact numbers of presented frames |

**`status`** `{fps_window_ms?}`. It reads the title and program id that `Load` saw, the build from
`Common::g_build_fullname` and `g_scm_rev`, and `System::IsPaused`. It measures fps from
`Core::g_nx2_present_count` over a short window and does not call `GetAndResetPerfStats`, which
would reset the Qt status bar's counters. `g_nx2_present_count` counts Switch 2 presents, so
`presents`, `fps` and the input tools follow Switch 2 games only.

**`screenshot`** `{height?, save?, timeout_s?}`. It requests a 16:9 layout from
`Layout::DefaultFrameLayout`, and the renderer blits the presented framebuffers into it. The BGRA
result is turned into RGB and encoded as PNG with stb. The pixel buffer is owned by the completion
callback, so a request that times out cannot leave the renderer writing into freed memory. If the
Qt screenshot hotkey has a request pending, the tool fails instead of taking over its request.

**`log_tail`** `{after?, limit?, contains?}`. A `RingBackend` in `src/common/logging.cpp`
(`common/log_ring.h`) keeps the last 20,000 formatted lines. It is an ordinary log backend that
does nothing while its capacity is 0, the default; the service turns it on at boot and off when it
stops. A call without `after` returns the newest lines; `next` pages forward, so an agent can poll
"everything since my last look". Lines logged before the game finished loading are only in
`eden_log.txt`.

**`log_filter`** `{filter}`. It parses the filter with `Filter::ParseFilterString` and applies it
with `SetGlobalFilter`, the call the settings dialog makes. Lines already filtered out were never
formatted, so the new filter applies only to later lines.

**`capture_frame`** `{sampled?, buffers?, timeout_s?, manifest_lines?, index?}`. It reuses the frame
dump (below). `FrameDump::Arm` points the next capture at a fresh directory, so
`EDEN_NX2_FRAME_DUMP` is not needed. When to capture is decided by `FrameDumpSchedule`, which has no
Vulkan state and is unit-tested; it handles both the env triggers and the debug server's requests
and sets `RecordingSampledTextures`, which the pipelines check before they keep the textures each
draw and dispatch samples. With `buffers: true` the capture also records the uniform and storage
buffers bound for every draw and dispatch and each dispatch's group counts. At runtime `sampled`
and `buffers` are separate, so diffing a constant buffer across two captures does not pay for
every texture readback. The writer thread reports completion after it writes the manifest summary.
The tool returns the directory, the frame and summary lines, counts per entry kind, the first
entries and the `frame_dump_index.py` command; with `index: true` the adapter runs it.

**`sample`** `{threads?, seconds?, max_chars?}`. The in-process sampling profiler
(`src/common/nx2_sampler.cpp`; `eden-cli` also starts it at launch with `EDEN_NX2_SAMPLE`) on
demand: it pauses the named threads (default `GPU,GPUBack`) about 1000 times a second for `seconds`
(default 10) and returns self, self-and-caller and inclusive counts per function plus the hottest
instructions as module offsets. The report and its folded stacks go to `samples-<session>-<n>.txt`.
Windows only; the threads run slower while sampled, so read fps from a run without it.

**`reject_shaders`** `{hashes?, reject?, timeout_s?}`. `EDEN_NX2_REJECT_HASH` at runtime, with the
same keys (native program key or shader hash) and the same effect:

1. The lowering (`GenericEnvironment::LowerNativeCode`) checks the runtime list right after the env
   list. A rejected program throws the same `Shader::RuntimeError` as an unsupported one, so the
   existing rejection path handles it: the draw is skipped, the error is logged once and the
   address is remembered.
2. `ShaderCache::RefreshDebugRejections` runs on the rasterizer thread. It finds cached
   `ShaderInfo`s of lowered native programs whose `unique_hash` or `native_program_key` is on the
   list and drops them through the `InvalidatePagesInRegion` and `RemovePendingShaders` path that
   guest writes to shader memory take. For native code `unique_hash` is the shader hash
   (`GenericEnvironment::Analyze` returns `NativeShaderHash(code)`).
3. It sets `Dirty::Shaders` on every active channel, so the next draw looks the program up again,
   lowers it again and hits the rejection. Compute looks its program up on every dispatch.
4. After an un-reject every remembered rejection (`rejected_native`) is cleared, so programs
   rejected earlier are lowered again; programs that really fail are rejected again and logged
   once. The lowered result is identical, so the old pipeline is still in the pipeline cache under
   the same key and is reused: restoring costs nothing.

The tool waits until the rasterizer applied the change and reports `applied`. A bisection is a
loop: reject half of the candidates, screenshot, compare. Runtime rejections last for one boot.

### Guest and GPU memory

Reads return at most 64 KiB. Addresses are hex strings (`"0x8012345000"`) or JSON numbers, and
results give them back as hex strings.

**`read_memory`** `{address, size, as?}` reads the application process's guest virtual memory.
`as` is `hex` (default, one string), `u32`, `u64` (hex strings, since JSON numbers lose bits past
2^53) or `f32` (`"nan"`, `"inf"` and `"-inf"` for non-finite values). If any page of the range is
unmapped, the call fails and names the first unmapped address.

**`search_memory`** `{pattern, start?, end?, max_hits?, regions?, budget_ms?}` finds a byte pattern
(hex pairs in memory order, so little endian: `1.0f` is `0000803f`; up to 256 bytes) in mapped
guest memory. `regions` is `heap` (default) or `all`; `start` and `end` narrow it. Matches may
cross page boundaries but not unmapped gaps. It returns at most `max_hits` (default 64, at most 256)
addresses and stops after `budget_ms` (default 2000, at most 10000), then returns
`complete: false` and `next`; pass `next` as `start` to continue. A heap scan copies every mapped
page once and keeps one host core busy for up to the budget: run it once, not in a loop.

**`write_memory`** `{address, hex | f32}`, for experiments only. It refuses unless the emulator was
started with `EDEN_NX2_DEBUG_WRITE=1` (read once by the service at boot; `status` shows
`write_memory`). It writes 1 to 4096 bytes with `Memory::WriteBlock` from the server thread, logs
the write and returns the bytes that were there before; write them back when done. The game keeps
running, so a write can race the guest, and code it changes is not translated again.

**`read_gpu_memory`** `{gpu_address, size, as?, timeout_s?}` reads GPU virtual memory, for example
query report and semaphore buffers the game reads back. It runs on the rasterizer thread at the
next frame end, like a capture. It tries the bound channel's address space first, then every other
address space the rasterizer knows, and reads from the first one that maps the whole range
(`address_space` in the result is `Tegra::MemoryManager::GetID`). Otherwise it names the first
unmapped address in the bound channel's address space.

How the reads are safe:

- **Guest memory, on the server thread.** `ReadGuestMemory` resolves each page with
  `Memory::GetPointerSilent`, the lock-free lookup the guest CPU cores use: an entry that was never
  committed reads as unmapped, and it never logs or downloads from the GPU (`ReadBlock` would do
  both, and its GPU flush may only run on the rasterizer thread). A mapped entry points into the
  `DeviceMemory` backing, which stays mapped for the whole boot, so a page unmapped during the copy
  gives stale bytes instead of a fault. `GetPointerSilent` masks the top 16 bits, so addresses at or
  above 2^48 count as unmapped instead of aliasing low memory. The service stops before the process
  is destroyed, so the page table outlives every read.
- **What a guest read can show.** Guest threads keep running, so a value can change or tear while
  it is copied; read it twice when that matters. On pages the GPU caches track, the read returns
  the CPU copy: GPU writes not written back yet are missing. `capture_frame` with `buffers` shows
  the GPU side.
- **GPU memory, on the rasterizer thread.** The server thread cannot know whether an address space
  is still alive, so it does not touch them. `TickFrame` collects the bound `gpu_memory` and the
  `address_spaces` under `config_mutex`, then calls `IsFullyMappedRange` and `ReadBlockUnsafe`, the
  reads the GPU itself makes. It does not flush the GPU caches.

### Input

**`input`** `{buttons?, left_stick?, right_stick?, frames, timeout_s?}` holds the buttons and sticks
for `frames` presented frames and then releases them; it returns when they are done.
**`input_sequence`** `{steps: [{buttons?, left_stick?, right_stick?, frames}, ...], timeout_s?}`
runs the steps back to back, at most 1000 steps and 36000 frames per call. A step with no buttons
and no sticks injects nothing, so it works as a pause between moves.

- **Buttons** are `A B X Y L R ZL ZR Plus Minus Up Down Left Right StickL StickR`. Home and Capture
  are not offered: they would leave the game. Unknown names and step keys are refused.
- **Sticks** are `[x, y]` in -1..1, up is +y, scaled by `HID_JOYSTICK_MAX`.

`EmulatedController::SetInputInjector` installs a function that `GetNpadButtons` and `GetSticks`
call while it is set. Injected buttons are ORed into the owner's, and an injected stick replaces the
owner's stick and its direction bits (threshold half of full scale). It changes no mapping, profile
or setting. The input goes to player 1, or to the handheld controller when player 1 is not
connected; the result names which. With no injector installed each getter costs one atomic load.

Timing is by presents, not wall time: the script starts at `g_nx2_present_count + 1`, and the
injector looks the active step up from the present count at every read, so the same sequence moves
the camera the same way on every run. The injector returns nothing from the end of the last step
on, and the handler removes it on every exit path: done, timeout (frames stop for `timeout_s`,
default `frames / 20 + 10`), shutdown, or client disconnect (a non-blocking peek at the request's
socket, polled every 2 ms). Injected buttons add to whatever the owner presses, so use it when the
controller is put down.

## Frame dump

`src/video_core/renderer_vulkan/vk_frame_dump.cpp` reads back one whole frame: the color targets of
every pass, the storage images written by every compute dispatch and the presented image, as
`.raw.zst` plus PNG previews (float targets encoded to sRGB, NaN and Inf in magenta) and a
`manifest.jsonl` with draw and dispatch indices, shader hashes, formats, sizes, GPU addresses and
per-channel min/max/mean/NaN/Inf. Readbacks are encoded and written on a worker thread; staging is
drained every 256 MiB and a capture stops at 2 GiB (`"truncated": true` in the summary).

- From startup: `EDEN_NX2_FRAME_DUMP=<dir>` with `EDEN_NX2_FRAME_DUMP_AT=<seconds>` (default 60),
  `<frame>f`, or `file` (captures once `<dir>/capture.now` exists, checked every 30 frames, so a
  frame can be picked while playing). `EDEN_NX2_FRAME_DUMP_SAMPLED=1` adds every texture a pass
  samples (3D and depth included, once per pass) and each compute shader samples before its first
  dispatch, with the sampler state and raw TSC words (`kind: sampled`, stage 5 = compute), plus the
  bound buffers below.
- At runtime: `capture_frame`, where `sampled` and `buffers` are separate options.
- Bound buffers: the uniform and storage buffers of every draw and dispatch, once per shader and
  binding (`kind: buffer`, with stage, binding, GPU address and size; bytes deduplicated in
  `buffer_<hash>.bin.zst`, copied on the GPU when the GPU wrote the range), and each dispatch's
  group counts (`kind: launch`).
- Every list of shader hashes (`"shaders"`) has the native program keys of those shaders next to it
  (`"keys"`, same order, `""` where the shader is not a native program or its key is unknown);
  buffer entries have `"shader"` and `"key"`.
- `py -I tools/nx2/frame_dump_index.py <dir>` builds an `index.html` contact sheet. Find the first
  pass whose output is wrong, then look at its shaders.

A runtime capture uses up an `EDEN_NX2_FRAME_DUMP_AT` capture that has not fired yet: they share
one `FrameDump`. Vulkan only.

## Rejecting programs from startup

`EDEN_NX2_REJECT_HASH=<hash>,...` rejects native programs by native program key (shader dump file
names, `diag-state.json` `key`) or shader hash (frame dump `shaders`, `diag-state.json` `hash`),
which stay the same between runs, unlike the addresses `EDEN_NX2_REJECT` takes. The log shows
`rejected by EDEN_NX2_REJECT_HASH` per program. Bisect by halving the list, or with
`reject_shaders` without relaunching. The key covers the captured code, so recompute it when the
capture rules change.

## Limits

- Vulkan only for `capture_frame`, `reject_shaders` and `read_gpu_memory`: the drain point is in
  `RasterizerVulkan::TickFrame`.
- One request at a time. A second client waits while a capture runs.
- `status` fps, `input` and `input_sequence` count Switch 2 presents (`g_nx2_present_count`).
- Not implemented (later phases in Lichii's design notes): pause, resume and frame stepping, a
  shader inventory, live switches, single texture or buffer dumps, per-pass GPU timings. The core
  cannot save state.
