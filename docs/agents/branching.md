# Branch grammar

Branch names are the repository's index. They are produced by a script, checked
by four independent layers, and parsed by the tooling, so nobody has to maintain
a "work in flight" list by hand.

## Grammar

| Kind | Form | Purpose | Lifetime |
|---|---|---|---|
| Work | `work/<owner>/<area>/<issue>-<slug>` | normal change, claimed on an issue | until merged or closed |
| Experiment | `exp/<owner>/<area>/<issue>-<slug>` | time-boxed spike that may fail | issue must state `decide by <date>`, max ~3 weeks |
| Agent scratch | `ai/<tool>/<issue>-<slug>` | an agent's own branch | deletable once its PR closes |
| Integration | `int/<area>` | long-lived merge target for one subsystem | until the feature lands default-off |
| Upstream sync | `sync/upstream-<yyyymmdd>` | pulls, reworks, revert experiments against Eden | days |
| Release | `rel/<x.y.z>` | release prep, cut from `master` only | days |

`<owner>` is a GitHub login, lowercased. `<area>` is one of:

```
vulkan shader core hle android qt audio input network cmake ci docs meta
```

`<tool>` is one of: `claude codex amp opencode gemini copilot other`.

A **segment** is `[a-z0-9]+(-[a-z0-9]+)*`: no leading, trailing or doubled dash.
The exact patterns live in `tools/agents/lib.sh` (`agent_branch_regex`), which is
what the scripts, the git hooks and CI all call. One definition, four callers.

```sh
. tools/agents/lib.sh && agent_branch_regex     # print them
agent_branch_valid work/kraibse/vulkan/4610-dynstate-v3 && echo ok
```

Banned, with examples from this repository's history: any name without a kind
prefix (`funny-meows`, `civa`, `dmnt2`, `sjkdbsdfjkbsdf-2834`), and any name that
encodes a negotiation (`2ndAdjustmentVulkanV3-STOP_FORCE_PUSHING_CAMILLE`). The
second kind is a symptom: it means branch ownership was undefined, so people
fought over a shared ref. Fix the rule, not the name.

## Creating a branch

```sh
tools/agents/branch.sh 4531 nce-readback --area core
tools/agents/branch.sh 4610 dynstate-v3 --kind exp --area vulkan
tools/agents/branch.sh 4611 lsfg-quicktoggles --kind ai --tool claude
```

The script validates the issue (open, readable), derives the name, cuts it from
`origin/master` (`--base int` continues a subsystem), creates
`<main checkout>/.worktrees/<branch>`, sets `branch.<name>.description` from the
issue title, keeps `.worktrees/` out of `git status`, and prints the next
commands. It refuses to create a branch whose name the grammar would reject.

**The no-issue lane.** Issue `0` means "no issue: the PR is the record". It is legal
only in a self-describing area -- `cmake`, `ci`, `docs`, `meta` -- where the change is
decidable from the diff alone. Behaviour areas (`vulkan`, `core`, `hle`, `android`, ...)
always need a real issue, because why the change exists is not visible in the diff.
`branch.sh`, `verify.sh` and `gate/branch-name` enforce the same rule.

When a no-issue change outlives one PR, open the issue *then* -- `gh issue create`,
body starting `Extracted from #<PR>` -- and give the follow-up work a real number. The
merged PR stays the record for the part that already landed.

## Four enforcement layers

| Layer | Artifact | What it stops |
|---|---|---|
| Creation | `tools/agents/branch.sh` | nobody types a name, so drift is impossible |
| Local gate | `tools/agents/verify.sh` | refuses to run unless the current branch conforms, so a hand-named branch cannot produce an accepted result |
| Hooks | `.githooks/pre-push` (installed by `tools/agents/setup.sh`) | publishes only conforming names; blocks force-push to `master`, `int/*`, `rel/*`, `sync/*` and to branches whose owner segment is not you |
| Server | `gate/branch-name` required check, plus a weekly audit | a non-conforming PR cannot merge; abandoned/expired branches are reported in one issue |

GitHub cannot prohibit branch creation by pattern. The enforceable equivalent is
this: PR-time check + pre-push refusal + a report. Do not expect a server-side
pattern rule to exist.

## Claims

One branch = one owner, and one record: an issue, or the PR itself when the branch
carries issue `0`.

```sh
tools/agents/claim.sh                  # claim everything this branch changes
tools/agents/claim.sh path/to/file.cpp # claim specific files
tools/agents/claim.sh --dry-run        # print the comment, post nothing
```

`claim.sh` posts a `_CLAIM_` comment listing the expected files, and — more
usefully — prints which other active branches already touch them. That report is
cheap and it runs before the conflict exists.

## Lifecycle

- `exp/` must carry a `decide by` date in its issue. The weekly audit flags
  overdue ones: promote to `work/` on the same issue, or delete.
- `int/<area>` may be temporarily red on tests (say so in the issue), never on
  compile. Features drain from it into `master` as default-off chunks.
- `ai/` branches are scratch: they may be deleted as soon as their PR closes.
- `HANDOFF.md` is tolerated only on `exp/` and `ai/` branches and must be gone
  before merge.

## Migrating 164 legacy branches

Do **not** rename refs collectively. That breaks every collaborator's local
remote-tracking refs for no gain, and the history of what the branch was for
would still not exist.

1. Legacy-named branches are frozen: no new commits.
2. Any branch that needs more work gets an issue and a conforming branch cut
   from trunk (`status.sh` marks them `state=legacy`).
3. Merged or superseded legacy branches are deleted by the audit action.
4. The name of the old branch goes in the new issue body, so the reason survives
   the rename that never happened.

## Seeing the state

```sh
tools/agents/status.sh              # table: age, area, owner, issue, state, ahead/behind
tools/agents/status.sh --write      # regenerate docs/agents/work-in-flight.md
tools/agents/status.sh --conforming-only
```

Legacy names are listed on purpose — the graveyard is the finding.
