# Agent providers

One canonical instruction file, many harnesses, one gate. This page is about the
second and third parts: what each provider may do here, and how it is wired.

## Adapters are pointers

| Harness | File | Mechanism |
|---|---|---|
| Claude Code | `CLAUDE.md` | `@AGENTS.md` import |
| Codex, Amp, opencode, recent Cursor | `AGENTS.md` | read natively, nothing to add |
| Cursor rules | `.cursor/rules/agents.mdc` | pointer, `alwaysApply: true` |
| Copilot | `.github/copilot-instructions.md` | pointer text; it ignores imports |
| Gemini CLI | `GEMINI.md` | pointer text (context-heavy) |

`gate/lint` fails if any adapter exceeds 8 lines or stops mentioning `AGENTS.md`.
That single check is what keeps one file canonical as providers multiply. If an
adapter needs to say more, the content belongs in `AGENTS.md`.

## Capability matrix

| Capability | Any harness | Caveat |
|---|---|---|
| Read, search, analyse, plan | yes | — |
| Edit in its own worktree, on its own branch | yes | `.worktrees/<branch>`, `build/<preset>` |
| Run `verify.sh` | yes | serialised by `flock`; parallel compiles read as flaky failures |
| Commit | yes, with `Assisted-by:` | the human owns the final message text |
| Push | only to `ai/*` or its own `work/*` branch | pre-push hook enforces it; `--force-with-lease` only |
| Open/comment on this repo's issues and PRs | yes | human-confirmed text; never on upstream Eden |
| Merge / approve / mark ready | **never** | — |
| Propose diffs without a compiler (chat, inline completion) | yes | **review-only**: may not land |

## Model tiering

Tier by subsystem risk rather than by provider:

- **Cheap/fast models**: mechanical sweeps, mass renames, CMake list edits, docs,
  locale-neutral cleanups, generating the skeleton of a setting addition.
- **Strong reasoning models only**: `src/video_core/renderer_vulkan/*`,
  `src/shader_recompiler/*`, `src/core/hle/*`, `src/core/arm/*`. These are where
  "wrong but plausible" is the failure mode, and a cheap model there produces
  exactly the class of change `docs/policies/AI.md` forbids.

A review-only provider is still useful on the hard subsystems: it can read a hot
file, explain the pipeline-key interaction and propose a diff a human then builds
and tests.

## Isolation

- One branch, one worktree, one build directory. Two agents in one checkout fight
  over the git index and over `build/`.
- `tools/agents/verify.sh` takes an exclusive lock at `.git/agent-build.lock`
  around configure/build/test. Hand-rolled `ninja` skips it and is how "the build
  is flaky" reports start.

## Adding a provider

1. Add a pointer file to the table above (≤ 8 lines, mentions `AGENTS.md`).
2. Add its token to `AGENT_AI_TOOLS` in `tools/agents/lib.sh` if it needs an
   `ai/<tool>/...` branch kind.
3. Nothing else. If the provider needs its own rules, the rules are wrong for the
   other providers too, so they belong in `AGENTS.md`.

## Known limits worth stating in the open

- Copilot gives no reliable way to run a compiler; it is review-only here.
- Chat-only harnesses cannot see build logs unless they are pasted; use
  `verify.sh` output as the paste.
- Any provider that "just opens a PR for you" is a policy violation here: text and
  authorship are the human's.
