# Project skills

Copies of agent skills that are relevant to this repository, in the layout
Claude-style harnesses load from a project: `.claude/skills/<name>/SKILL.md`, with
any `references/` and `scripts/` alongside.

These are **copies**. They are committed so an agent working from a bare checkout
has the same procedural knowledge as one running on a developer's machine, where
the originals live. They are not authoritative: if a copy and an original
disagree, the original wins — refresh the copy.

| Skill | Origin | Why it is here |
|---|---|---|
| *(none vendored yet)* | | |

## Adding one

```sh
mkdir -p .claude/skills
cp -r "$HOME/.hermes/skills/gaming/switch-emulator-diagnostics" .claude/skills/
```

Then add a row to the table above: skill name, where the original lives, and one
line on why this repository needs it. A skill with no "why" is a skill nobody
will load.

Worth vendoring for this tree, in rough order of value:

- `switch-emulator-diagnostics` — reading run logs, TIPC command buffers, probe
  discipline; the only "does it work" loop the suite does not cover.
- `cmake-build-link-diagnostics` — CPM, `.patch/` overrides, `-- -k 0` failure
  collection.
- `patch-branch-handoff` — how to write the state brief a long branch needs.

## Refreshing

```sh
for s in gaming/switch-emulator-diagnostics \
         software-development/patch-branch-handoff; do
    rm -rf ".claude/skills/$(basename "$s")"
    cp -r "$HOME/.hermes/skills/$s" ".claude/skills/"
done
```

## Conventions

- Nothing here is compiled, packaged or shipped; it is agent context only. Say so
  if a reviewer objects to it in a build.
- `.codex/skills/` is accepted as a mirror location for harnesses that look there;
  keep `.claude/skills/` canonical and do not maintain two divergent copies.
- Harness state (sessions, local settings, caches) is gitignored — see the
  `.claude/*` / `.codex/*` block in `.gitignore`. Only `skills/` is excepted.
