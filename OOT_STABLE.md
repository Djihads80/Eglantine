# Ocarina of Time: stable, playable state (2026-11-05, updated 2026-11-06)

> [!WARNING]
> **This is a RESULT FIRST fork.** Changes land here once they work in game, measured and tested, before anyone else has reviewed the code. Much of it is AI-assisted and has not been peer-reviewed. Expect rough edges, and don't take a change here as accepted by upstream Eglantine. Code meant for upstream goes through its normal review there.

Branch `stable/oot-playable-no-crash-2026-11-05` freezes the first state of `local-combined` that
plays The Legend of Zelda: Ocarina of Time (Switch 2, `0400486024DEC000`) without crashing.
`local-combined` has moved on since: the flash fix and the switch system below are not on the
stable branch.
Test system: Windows 11, Ryzen 9 9950X3D, RTX 5090, GPU accuracy Low, GPU thread split on.

- **How we got here:** [OOT_WORKFLOW.md](OOT_WORKFLOW.md), the method in one page.
- **Who made it:** [CREDITS_OOT.md](CREDITS_OOT.md).
- **Want to help?** [OOT_HELP_WANTED.md](OOT_HELP_WANTED.md).

## What works (stable branch, build `1fa3108afb`)

- **No device loss.** 13 automated runs through the Kokiri Forest save load and a play session,
  where every earlier build lost the Vulkan device about 2 s after the load.
- **Kokiri Forest: about 51 FPS median** in the automated run, at the first lab save's spot, and
  40-60 in play. That number is not comparable with the ones below, which use a later save.
- **Normal game speed below 60 FPS.** The built-in "Real-time speed" patch (Add-Ons) replaces
  the game's fixed 60 FPS frame step with the measured one, so dips no longer play in slow motion.
- No visual glitches in a short play session. Longer play showed **brightness flashes**: see
  below. On the stable build, `EDEN_NX2_PERF_FRAMES_AHEAD=0` should avoid them (the same cause;
  tested only on the later build).

## The changes that matter

| Commit | Change |
|---|---|
| `43b6b17c3a` | Clear a deleted buffer's id in every GPU channel. Ocarina draws from four; stale ids pointed descriptors ~100 MiB past recycled buffers and the GPU faulted. |
| `b5a01b8e98` | Preflush GPU-written memory the CPU reads, native titles included (37 -> 52 FPS). `EDEN_NX2_PREFLUSH=0` restores read traps. |
| `bc34d467b6` | Early macro DMA reads outside GPU-written 1 MiB regions skip the full split drain. Verified with `EDEN_NX2_PERF_VERIFY=1` (7 stale reads with 4 KiB tracking, 0 with 1 MiB). `EDEN_NX2_PERF_OFF=macro_read_early` turns it off. |
| `5f795179b0` | Reports and fence releases mark their pages, so early reads of occlusion results wait for them (costs ~4 FPS; the rest was stale data). |
| `2c5caba758` | Real-time speed patch (lichii #216). |
| `35fa1ba02f`, `08b5ed82eb` | Wait for pending query results before macro reads at Low accuracy, bounded to 2 s. |

Also merged: lichii PRs #173, #174, #178 (part), #185, #189, #192, #199, #204, #206, the Frizti
writer check, and upstream #663 and 743d5bbaed.

## Since the stable state (`local-combined`, 2026-11-06)

- **Flashes fixed.** `frames_ahead` (on in the stable build) lets Ocarina run a frame ahead of the
  GPU, so the game rewrites constants and exposure data the GPU thread still reads for the previous
  frame; only the command stream is copied at submit. `present_one_ahead` (added overnight, +9 FPS)
  did the same. Both are always off now (`e4dc818c91`, `a849da9a0f`). A burst of 30-40 captures
  standing still found 25-40% of frames at another brightness with `frames_ahead` on and none
  with it off, and the flashes are gone in play.
- **The cost: ~5 ms per frame.** At the current lab save's spawn spot: **~36 FPS** median, and
  **43-45 FPS** after the scripted walk. With the flashing switches on, the same spot measured
  ~46 (`frames_ahead`) and 55 (`frames_ahead` + `present_one_ahead`).
- **Kept from the overnight rounds** (each measured alone at the spawn spot, two runs):
  `submit_while_waited` (`8705321017`, submit while the front waits or the GPU idles; 38.5 -> 45.5
  FPS before the flash fix) and `bank_once_per_frame` (~+1.7 FPS). Also on by default: 14
  switches from the first wave (macro batching, early macro reads that wait only for writers,
  constant buffer overlay, download batching, preflush aging and more); together they measured
  within noise at the spawn spot. About fifteen others showed no clear gain, flashed or
  misbehaved, and stay off.
- **Switches.** Every experimental optimization has a name; the `Nx2Perf:` log line at start
  lists them with their state. `EDEN_NX2_PERF_OFF=a,b` and `EDEN_NX2_PERF_ON=a,b` set them by
  name, `EDEN_NX2_PERF_SAFE=1` turns all experimental ones off, `EDEN_NX2_PERF_OPT=0` turns off
  every one. `EDEN_NX2_PERF_CONTROL=<file>` flips the live-safe ones while the game runs (`label`,
  `on`, `off`, `safe` lines; each applied file is logged as `Nx2PerfLive: set=...`).
- **Frame statistics.** One `Nx2Frame:` log line per second: present interval p50/p90/p99/max,
  time blocked on the GPU fence, and how many intervals took over 20 ms. `EDEN_NX2_FRAME_STATS=0`
  turns it off.
- **Data migration dialog** (`c7412bd112`, from upstream): "Keep Old Data" moved the old data
  folder instead of copying it, and "Clear Old Data" linked it. Each choice now does what it says.

## Still open

- **Locked 60 FPS.** ~36 FPS at the spawn spot means ~11 ms per frame to find. The largest known
  piece is the ~5 ms `frames_ahead` gave: running ahead safely needs the data the GPU thread reads
  late to be copied or protected per frame. Then the front's waits (one macro's DMA reads,
  command-list writers, semaphore acquires) and the back's per-draw setup. Details:
  [OOT_HELP_WANTED.md](OOT_HELP_WANTED.md).
- **3D V-Cache pinning is unmeasured.** On Ryzen X3D CPUs the emulation threads are pinned to the
  V-Cache CCD ("Pin Emulation Threads to 3D V-Cache CCD" in the CPU settings, `prefer_3d_vcache`;
  `EDEN_NX2_VCACHE=0` or `=1` overrides it). It is on by default, also on the stable branch, with
  no A/B yet. It goes off by default unless it wins.
- Vulkan validation layers flag `vkCmdEndQuery` outside a render pass, an image view format not in
  the image's format list (A8B8G8R8_UNORM_PACK32), and `SPV_EXT_shader_stencil_export` used without
  the extension.
- `split_overlap` (early command-list fetches) stays quarantined: unverified, and it once lost
  the device.
- The stable build renders the current lab save's spawn spot wrong (garbage frames); `local-combined`
  renders it right. Not investigated.
- One unexplained 60 s freeze in one of 14 lab runs, not reproduced since.

## Reproducing the measurement

Use the lab in [tools/nx2/lab](tools/nx2/lab/README.md). The number to compare is the median of
the 600-present windows at the save's spawn spot (`spawn` in `perf_summary.py`), and separately
after the walk, from the same save on both sides.

AI assistance: the changes in this state were made with Claude (Fable 5.1 and Opus 5.5) and are
marked with `Assisted-by:` trailers; earlier commits on this line carry `Co-Authored-By` lines
from other tools.
