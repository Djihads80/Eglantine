# Agent and contributor docs

Two files are upstream-owned and must not be extended here: `AGENTS.md` at the root
(a policy pointer) and `docs/policies/*.md`. Policy additions go into
`docs/policies/AI.md`, which carries an Eglantine-local addendum; operational context
lives in this directory.

| File | Owns |
|---|---|
| `AGENTS.md` | policy pointer, upstream-owned — leave it alone |
| `docs/policies/AI.md` | AI policy, including the Eglantine agent addendum |
| `docs/policies/Coding.md` | code style |
| `context.md` | what this tree is, build/test gate, how work enters, where work stops |
| `build.md` | per-platform build truth, presets, known failure modes |
| `branching.md` | branch grammar, claims, lifecycle, legacy migration |
| `providers.md` | which harness may do what, model tiering, isolation |
| `disclosure.md` | disclosure forms and what agents may never author |
| `hot-files.md` | **generated** — collision map from history and live branches |
| `work-in-flight.md` | **generated** — branch/issue table |
| `.claude/skills/` | vendored project skills (`README.md` there has provenance and the refresh loop); `.codex/skills/` is an accepted mirror |

`hot-files.md` and `work-in-flight.md` are produced by tools, never edited by hand.

## Tooling

| Command | Purpose |
|---|---|
| `tools/agents/setup.sh` | once per clone: `core.hooksPath`, executable bits, `.worktrees/` exclude |
| `tools/agents/branch.sh` | create a conforming branch with its worktree |
| `tools/agents/claim.sh` | claim files on the issue; report overlap with active branches |
| `tools/agents/verify.sh` | the gate: name, build, tests, headers, spelling, TEMP audit |
| `tools/agents/known-failures.txt` | the test baseline: cases that already fail on master, with `[flaky]` marking the intermittent ones |
| `tools/agents/status.sh` | what is in flight, parsed from branch names + issues |
| `tools/agents/hot-files.sh` | regenerate `hot-files.md` |
| `tools/agents/lib.sh` | shared grammar and helpers; sourced, never executed |

Regenerate the derived docs after a batch of merges:

```sh
tools/agents/hot-files.sh
tools/agents/status.sh --write
```

Both files are safe to regenerate in a PR: they are produced from `git` and the
tracker, so two people regenerating them converge instead of conflicting.
