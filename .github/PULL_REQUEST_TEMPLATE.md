Closes #

## What this changes, and why

<!-- Human text. Two or three sentences: the bug or feature, and the mechanism.
     Not a file list, and not a generated summary. -->

## AI disclosure

<!-- docs/policies/AI.md: disclosure must appear here AND as a trailer in the commits. -->

- [ ] No AI or LLM tool was used for this change
- [ ] AI was used and is disclosed: `Assisted-by: <model> (<what it was used for>)`

## Verification

- [ ] `tools/agents/verify.sh` passes (paste the RESULT line and preset below)
- [ ] Built and tested on the platforms this touches: Linux / Windows (say which)
- [ ] For a renderer or HLE change: attached a run log showing the behaviour

Preset(s) used:

```
```

## Housekeeping

- [ ] Branch follows the grammar (`tools/agents/branch.sh`), one issue, claimed in the issue
- [ ] Branch is up to date with `master` (scratch rebase, never a shared branch)
- [ ] New setting? Chosen a lane: permanent setting / `debug_knobs` bitmask / TEMP toggle
- [ ] New `TEMP(...)` markers? Each links a cleanup issue ( # )
- [ ] Registries edited append-only (no reordering, no reformatting)
- [ ] No edits to `dist/languages/**`, non-English `values-*/`, `externals/**`
- [ ] Commit messages are my own text, namespaced (`[video_core] ...`)
- [ ] No file here is also being edited by another open PR, or the overlap is noted below

## Overlap with other PRs

<!-- the gate/overlap job comments this automatically; add what it cannot see -->

-------------------

I have read and followed the [contribution guidelines](CONTRIBUTING.md#code-contributions),
the [AI policy](docs/policies/AI.md) and the [coding guidelines](docs/policies/Coding.md).
