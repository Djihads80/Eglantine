# AI disclosure

`docs/policies/AI.md` is the normative document; this page is the operator's
summary of the parts that are mechanism. If they disagree, the policy wins.

## The trailer

```txt
Assisted-by: <model> (<what it was used for>)
```

Example, in a commit message:

```txt
[video_core] fix TOTK dark squares on RADV

The descriptor set was rebound between the vertex and fragment stages while
the pipeline still expected the pre-bound layout. Bind once and reuse.

Assisted-by: Claude Opus 5.5 (initial draft and review)
```

Rules that go with it:

- It goes in **every** commit of a series that will survive a squash, so the
  disclosure cannot be lost in the final squash body.
- The same disclosure appears as a sentence in the PR body:
  `I used <model> to help with <X>.`
- Multi-commit series keep the trailer per commit; maintainers handle merges.

## What an agent may and may not write

| Field | Agent | Human |
|---|---|---|
| Code | may draft, must be reviewed line by line | owns, must be able to defend it in review |
| Commit message | may draft | rewrites and owns the final text |
| PR description / issue body | may draft | rewrites and owns the final text |
| `Assisted-by:` trailer | may add to its own commits | confirms it |
| `Signed-off-by:` / `Co-authored-by:` | **never** | the human signs off |
| Review comments | may draft, never post verbatim | posts |

Verbatim model output in commit messages, PR descriptions, issues and comments is
explicitly forbidden by the policy. That is why `.githooks/commit-msg` warns when
a `work/`, `exp/` or `ai/` commit has no trailer, and fails on a
`Co-authored-by:` naming a model.

## Also forbidden

- Pasting console keys, encryption material, private user data or embargoed
  vulnerability reports into a model.
- Automated PR/issue/comment floods. The policy calls this a denial-of-service on
  reviewers: one issue per claim, human-confirmed text, no batches of generated
  PRs.
- Verbatim AI code review (Codex/Copilot review comments pasted as a review).
- Submitting work you cannot explain.

## What is explicitly fine

- Research, codebase search, understanding the tree.
- Debugging, including with a model's help, as long as the fix works and you
  understand it.
- Reviewing your own change before opening a PR.
- Tedious-but-mechanical migrations where the pattern is already decided.

Generating boilerplate, writing unit tests, and large refactors where a model did
most of the implementation are in the "discuss first in an issue" bucket.

## Disclosure is not a permission slip

Quality is judged as if the change were hand-written. The gate decides whether it
builds on both platforms; a reviewer decides whether it is worth the review time.
