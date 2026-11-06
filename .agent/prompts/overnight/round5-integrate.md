# Round 5 integration (2026-11-06, finished ~14:50)

HEAD `4a8a8b5fda` on local-combined (merges 357c5d6d82, 3d17c7b9bd, 6aa9d7717d, 1fb8839c70; renumber 3681e89664).

Baseline (defaults, r5-base-a/b): pooled median 36.6 FPS, slowest window 34.1. Flash (r5-base-flash): luma median 0.2102, sd 0.0018, 0 outliers.

| switch | verify | flash | speed a/b (pooled, slowest) | decision |
|---|---|---|---|---|
| dma_forward | exact (1.57M calls, earlier) | 0 outliers, sd 0.0023 | 38.6/38.2 (38.5, 32.8) | ACCEPT, default on |
| draw_begin_coalesce | 0 non-matching, ~320k mergeable/s | not timing | 37.0/37.8 (37.6, 35.2) | ACCEPT, default on |
| guest_landed_syncpoints | diagnostic (H1' confirmed on old path) | 0 outliers, sd 0.0014 | 36.8/37.6 (37.5, 34.8) | reject (+0.9) |
| hook_lazy_flush | 9 skipped submits pending at fence waits | 0 outliers, sd 0.0017 | 37.6/37.0 (37.5, 36.0) | reject (+0.9, slowest +1.9) |
| war_guard_probe | no unexpected page states | n/a | not run (probe) | stays off (measurement) |

Confirmation with both accepted on (defaults): r5-confirm-a/b 36.9/37.2, pooled 37.2, slowest 32.8; flash 0 outliers, sd 0.0031 (slow drift, no brightness steps); game.png correct; no DEVICE_LOST. Net gain over base is +0.6 (within noise): the two gains did not stack.

Notes for round 6: the spawn spot is still bound by the serial guest/GPU structure (~37 FPS). guest_landed_syncpoints is the first step toward a flash-safe frames_ahead (guest no longer sees syncpoints before they land); try frames_ahead + guest_landed_syncpoints under the flash gate. The war_guard_probe log (r5-v-war_guard_probe, "t_w" lines) measures how long after vi2's reply the guest first touches guarded pages.
