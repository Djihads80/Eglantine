# Switch 2 titles and video memory

## Problem

Mario Kart World with uncompressed ASTC (Vulkan, AMD) shows garbage textures, then crashes.

- RX 6900 XT 16 GB: `Available VRAM: 14.21 GiB`, `Texture cache thresholds: expected 12088 MiB,
  critical 13727 MiB`, but during play `VMA heap 0: usage 13499 MiB of budget 13504 MiB`, then
  `Image ... is over the memory budget; allocating it anyway` and `does not fit; using an unbacked
  sparse image`: images without memory.
- RX 9060 XT 8 GB: `Available VRAM: 7.00 GiB`, thresholds `expected 7131, critical 7899`: critical
  above the card's memory. Same symptoms.
- With `astc_recompression=Bc3` the 16 GB card stays near 11 GB, no problem.

The collector's thresholds followed the sum of every heap's budget. On AMD the host-visible
device-local heap shares the same VRAM, so the sum (8.5 GB on the 8 GB card) is more than the heap
the images fill: the collector never got aggressive before allocations went past the budget.

## Changes

1. **Thresholds under the real budget** (`Device::GetDeviceMemoryBudget`, `TextureCache::TickFrame`).
   The budget is now the main device-local heap's (VK_EXT_memory_budget), plus what the other heaps
   use (the collector's usage figure counts them). For Switch 2 titles, every 30 frames:
   `margin = min(budget / 8, 1 GiB)`, `critical = budget - margin`, `expected = budget - 2 * margin`.
   13.5 GiB budget: critical 12800 MiB, expected 11776 MiB. 7 GiB budget: critical 6272 MiB,
   expected 5376 MiB. `EDEN_NX2_TC_EXPECTED_MIB` / `EDEN_NX2_TC_CRITICAL_MIB` still win.
   Switch 1 titles keep Eden's thresholds.
2. **BC3 on small cards**: a Switch 2 program on a GPU without native ASTC, with `astc_recompression`
   Uncompressed and less than 12 GiB of budget at boot, uses BC3 for the session (per-game value,
   the config file is not changed), with one warning in the log. `EDEN_NX2_KEEP_ASTC=1` keeps the
   uncompressed textures. The decision is taken when the Vulkan texture cache runtime is created,
   the first place where both the device's memory and the setting's readers meet (it builds the
   BCn encoder and the view formats from it); the core marks the program as Switch 2 before that.
3. **Small card test knob**: `EDEN_NX2_VRAM_LIMIT_MIB=<n>` limits every device-local heap to n MiB:
   VMA's `pHeapSizeLimit` (allocations past it fail, like on a smaller card), the budget the texture
   cache follows and the `Available VRAM` figure. Logged at startup.
4. **Images without memory count for nothing** (`TextureCache::ImageMemoryBytes`). Eglantine gives
   sparse regions memory only where the GPU writes: an image over unwritten blocks has no device
   memory, and a large terrain array gets it layer by layer (`Image::BindLayers`, at most 768 MiB,
   384 MiB under 7 GiB). The collector compares the driver's figure, which counts what is bound,
   to the thresholds above; between two readings it keeps its own estimate, which counted every
   image at its full size, so deleting one without memory looked like freeing gigabytes, or took
   the total below zero. Such an image now counts for what is bound to its layers, added as it is
   bound.

Automatic BC3 does not change the terrain arrays: huge ASTC arrays stay on the GPU decoder as
RGBA8 with trimmed levels. `EDEN_NX2_VRAM_LIMIT_MIB` under 7168 also selects the small-card cap
for terrain arrays, and a layer whose allocation fails past the limit stays empty (logged).

## How to test

On a big card, reproduce an 8 GB one:

```
set EDEN_NX2_VRAM_LIMIT_MIB=7168
eglantine.exe
```

Expected in the log: `EDEN_NX2_VRAM_LIMIT_MIB: device-local heaps limited to 7168 MiB`, the BC3
warning at boot, then `Texture cache thresholds: expected ~5376 MiB, critical ~6272 MiB`, and no
`unbacked sparse image` during a race. With `EDEN_NX2_KEEP_ASTC=1` added, the textures stay
uncompressed and the collector works against the same thresholds (the limit is hard: past it,
images go unbacked, which reproduces the original symptoms if the collector cannot keep up).
