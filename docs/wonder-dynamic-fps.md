# Wonder frame-rate targets

The per-game System settings expose a numeric frame-rate target (1 to 1000,
default 60) for Mario Wonder Switch 2 Edition, program `040015100B514001`.
A restart is required. The loader checks build ID
`EC47E6F8846D2A7B6892E2682845C7255F7117AE` and all three original instructions
before patching. Unsupported builds are left unchanged.

The fixed patch changes both simulation rate and present interval. Native
presentation uses the rate actually patched at boot, so changing the setting
while playing cannot change the cadence without updating simulation. Targets
above 60 permit Mailbox/Immediate presentation where the device supports it.
The hardware must sustain the selected target to maintain full game speed.

## Experimental dynamic timing

Set `EDEN_WONDER_DYNAMIC_FPS=1` before launching to enable bounded game-time
feedback. This remains opt-in. The configured target still limits presentation.
Private timing hooks replace two instructions in the validated main module.
They return the simulation rate in W1; the fallback also updates the original
controller field. No executable memory is rewritten during gameplay.

The controller compares the engine time-scale readback with host elapsed time,
adjusted for emulation speed. It bounds phase error to 50 ms and corrects 20%
per sample. The physics step stops increasing at 30 Hz, or the explicitly lower
target. Below this floor, slowdowns remain. Loading gaps, invalid readbacks,
pauses and target/speed changes reset feedback instead of accumulating catch-up.

Set `EDEN_WONDER_SPEED_AUDIT=1` for timing logs. With audit enabled, also set
`EDEN_WONDER_SPEED_CSV=1` for a CSV in the log directory. Audit totals separate
above/below-30-FPS intervals and report uncovered gaps. Engine time-scale
readback does not prove that every gameplay timer or audio clock is correct.
Below-40-Hz behavior and audio synchronization still need gameplay validation.

## Overworld texture cache

For this title, the cache can exceed its historical 5-GiB ceiling when a usable
native driver budget is available. Driver headroom, allocator slack and memory
pressure reserves still apply. Budget shrink can still cause collection and
hitches. Other titles and unavailable budgets retain the conservative ceiling.

## Validation

The `[wonder_fps]` tests exercise build/opcode rejection, fixed and dynamic
hooks, bounded correction, pauses, speed/target changes, fractional frame rates,
jitter, readback errors and audit coverage. The `[memory_budget]` test checks
cache thresholds with large, shrinking and small budgets.

Before marking this port ready for merge, compare the same save, settings and
route with and without the changes. Check walking, jumping, moving platforms,
rhythm events and audio as well as frame timing. Automated checks do not replace
this gameplay validation.
