# Credits: Ocarina of Time stable state

This branch stands on a lot of people's work. Everyone below has commits on this branch that are
not in `Djihads80/Eglantine` `nx2`, or is credited in one of those commits' messages. Names are
written exactly as git records them; a few may be the same person under two handles. If you're
missing or listed wrong, open an issue or tell the branch owner and it will be fixed.

The emulator itself is Eglantine, built on Eden and yuzu: thanks to everyone who worked on those.

## Branch owner and integration

- **AManWithAPlan** (68 commits): integration on `local-combined`, the Ocarina crash and
  performance work in this state, porting and conflict resolution of the PRs below, testing.

## Code authors (commits on this branch)

| Contributor | Commits | Notable work |
|---|---|---|
| kraibse | 35 | Ocarina diagnostics (flash watch, present bursts), native 64 KiB uniform bank re-uploads, FP16 texture packing |
| zortos293 / Zortos | 16 / 5 | Lichii perf line: command-list waits, early MME DMA read quarantine, `EDEN_NX2_PERF_OFF`, fragment barriers |
| Duck912 | 5 | Ocarina's end-of-frame network as a compact software HMMA, per-tap gather offsets, F2I fixes, buffer download ordering |
| Jonas | 4 | Native storage buffer pointers through subgroup broadcasts, preflush downloads, MaxContinuousRange cache, core idling |
| thibaut | 3 | |
| nyakuoff | 3 | |
| Zarigis | 3 | |
| Shrxmy | 3 | |
| Gavin Schiller | 3 | Deferred clears before texture copies (lichii #185) |
| Felipe Lima | 3 | |
| jpkto | 2 | |
| SmileyTatsu | 2 | |
| Matt Artist | 2 | |
| Kajcsi / Kaj | 2 / 2 | |
| Brandon Bunnelle | 2 | |
| Frizti | 1 | Mesh programs that output points or lines; the command-writer classification ported from Frizti's work |
| Frysix | 1 | One write per console log line on Windows (lichii #199); found the occlusion-result stale read (#179) |
| gunvalk | 1 | Real-time speed below 60 FPS (lichii #216) |
| chuppatricky | 1 | RED/SURED register shadowing |
| Dr. Sergio | 1 | LayeredFS mods for NUP titles |
| resadent, rdenno, cr1ckt, SinCuri0, Paulo Tumabing, Paulius, Inc0rporate, 999sian | 1 each | |

## Credited in commit messages

- **Franseven**, **Quixomatic** (co-authors): native uniform bank re-uploads and FP16 texture
  packing.
- **Mattscreative** (co-author): the software HMMA network.
- **RomainROCH**: the review that made deferred clears land only when a copy proceeds (#185).

## Pull requests included

Lichii and upstream PR numbers referenced in this branch's commits (some refer to older Eden/yuzu PRs): #59, #70, #84, #89, #100, #101, #112, #120,
#121, #124, #129, #138, #141, #142, #149, #161, #165, #173, #174, #178, #179, #185, #189, #192,
#199, #204, #206, #215, #216, #387, #494, #538, #544, #556, #663.

## Tools

Some commits were written with AI assistance and say so in their trailers: Claude (Fable 5.1,
Opus 5.5, Sonnet 5.5), Google Antigravity, and capy-ai. The people above reviewed, tested and
own those changes.
