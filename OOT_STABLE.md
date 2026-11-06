# Ocarina of Time: stable, playable state (2026-11-05)

Branch `stable/oot-playable-no-crash-2026-11-05` freezes the first state of `local-combined` that
plays The Legend of Zelda: Ocarina of Time (Switch 2, `0400486024DEC000`) without crashing.
Test system: Windows 11, Ryzen 9 9950X3D, RTX 5090, GPU accuracy Low, GPU thread split on.

- **How we got here:** [OOT_WORKFLOW.md](OOT_WORKFLOW.md), the method in one page.
- **Who made it:** [CREDITS_OOT.md](CREDITS_OOT.md).
- **Want to help?** [OOT_HELP_WANTED.md](OOT_HELP_WANTED.md).

## What works

- **No device loss.** 13 automated runs through the Kokiri Forest save load and a play session,
  where every earlier build lost the Vulkan device about 2 s after the load.
- **Kokiri Forest: about 51 FPS median** in the automated run, 40-60 in play.
- **Normal game speed below 60 FPS.** The built-in "Real-time speed" patch (Add-Ons) replaces
  the game's fixed 60 FPS frame step with the measured one, so dips no longer play in slow motion.
- No visual glitches seen in a short play session.

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

## Still open

- **Locked 60 FPS.** The split GPU thread's front is the bottleneck (idle < 1%). Its biggest
  waits: early macro reads waiting for all queued GPU work, command-list writers, semaphore
  acquires. Each fix risks stale draw data, so it needs a Verify run with zero mismatches.
- `split_overlap` (early command-list fetches) stays quarantined: unverified, and it once lost
  the device.
- One unexplained 60 s freeze in one of 14 lab runs, not reproduced since.

## Reproducing the measurement

The lab driver and notes live in the local `.agent/` folder (not in git). The number to compare
is the median of the 600-frame windows after the save load in `eden_log.txt`'s
`PresentClientBuffer` lines, at the Kokiri Forest spawn point.

AI assistance: the changes in this state were made with Claude (Fable 5.1 and Opus 5.5) and are
marked with `Assisted-by:` trailers; earlier commits on this line carry `Co-Authored-By` lines
from other tools.
