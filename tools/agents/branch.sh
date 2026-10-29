#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Create a branch that conforms to the repo's branch grammar, with its own
# worktree, plus the claim it belongs to. This is the only sanctioned way to
# start work: names are derived, never typed.
#
#   tools/agents/branch.sh 4531 nce-readback --area core
#   tools/agents/branch.sh 4610 dynstate-v3 --kind exp --area vulkan
#   tools/agents/branch.sh 4611 lsfg-quicktoggles --kind ai --tool claude
#
# Result: work/<owner>/<area>/<issue>-<slug> in .worktrees/<name>/, cut from
# origin/master (or --base int to continue subsystem work).

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tools/agents/lib.sh
. "$here/lib.sh"

usage() {
    cat <<'EOF'
Usage: tools/agents/branch.sh <issue> <slug> [options]

  <issue>            open issue number, or 0 in a self-describing area
                     (cmake, ci, docs, meta -- the PR is the record there)
  <slug>             lowercase words joined by dashes, e.g. nce-readback

Options:
  --kind work|exp|ai normal work (default), time-boxed spike, or agent scratch
  --area AREA        one of: vulkan shader core hle android qt audio input
                     network cmake ci docs meta (default: prompt)
  --owner LOGIN      GitHub login for work/ and exp/ (default: your gh login)
  --tool TOOL        claude|codex|amp|opencode|gemini|copilot|other (ai/ only)
  --base REF         "master" (default), "int" for origin/int/<area>, or a ref
  --bootstrap        deprecated no-op (issue 0 is the no-issue lane)
  --no-fetch         do not fetch origin before cutting the branch
  --dry-run          print what would happen, change nothing
  -h, --help         this text
EOF
}

issue=""
slug=""
kind="work"
area=""
owner=""
tool=""
base_kind="master"
bootstrap=false
do_fetch=true
dry_run=false

while [ $# -gt 0 ]; do
    case "$1" in
    -h | --help)
        usage
        exit 0
        ;;
    --kind)
        kind="${2:-}"
        shift 2
        ;;
    --area)
        area="${2:-}"
        shift 2
        ;;
    --owner)
        owner="${2:-}"
        shift 2
        ;;
    --tool)
        tool="${2:-}"
        shift 2
        ;;
    --base)
        base_kind="${2:-}"
        shift 2
        ;;
    --bootstrap)
        bootstrap=true
        shift
        ;;
    --no-fetch)
        do_fetch=false
        shift
        ;;
    --dry-run)
        dry_run=true
        shift
        ;;
    -*)
        agent_die "unknown option $1 (try --help)"
        ;;
    *)
        if [ -z "$issue" ]; then
            issue="$1"
        elif [ -z "$slug" ]; then
            slug="$1"
        else
            agent_die "unexpected argument $1"
        fi
        shift
        ;;
    esac
done

root="$(agent_repo_root)"
cd "$root"

# ---- validate ---------------------------------------------------------------

[ -n "$issue" ] && [ -n "$slug" ] || {
    usage
    exit 1
}

case "$issue" in
"" | *[!0-9]*) agent_die "issue must be a number, got '$issue'" ;;
esac
if [ "$bootstrap" = true ]; then
    agent_warn "--bootstrap is deprecated: issue 0 now means 'no issue' and is legal in ${AGENT_SELF_DESCRIBING_AREAS// /, }"
fi

if printf '%s' "$slug" | grep -Eq -- '--|^-|-$|[^a-z0-9-]'; then
    agent_die "slug '$slug' must be lowercase a-z0-9 with single dashes, no edge dashes"
fi

case "$kind" in
work | exp | ai) ;;
*) agent_die "kind must be work, exp or ai (got '$kind')" ;;
esac

if [ "$kind" = ai ]; then
    [ -n "$tool" ] || agent_die "ai/ branches need --tool (one of: $AGENT_AI_TOOLS)"
    case " $AGENT_AI_TOOLS " in
    *" $tool "*) ;;
    *) agent_die "unknown tool '$tool' (one of: $AGENT_AI_TOOLS)" ;;
    esac
else
    [ -n "$area" ] || agent_die "work/ and exp/ branches need --area (one of: $AGENT_AREAS)"
    case " $AGENT_AREAS " in
    *" $area "*) ;;
    *) agent_die "unknown area '$area' (one of: $AGENT_AREAS)" ;;
    esac

    if ! agent_issue_lane_ok "$area" "$issue"; then
        agent_die "issue 0 (no issue) is legal only in a self-describing area (${AGENT_SELF_DESCRIBING_AREAS// /, }); '$area' changes need a real issue, because the diff cannot show why"
    fi
fi

case "$kind" in
ai) name="ai/$tool/$issue-$slug" ;;
*) name="$kind/$( [ -n "$owner" ] && printf '%s' "$owner" || agent_owner_login )/$area/$issue-$slug" ;;
esac

agent_branch_valid "$name" || agent_die "derived name does not match the grammar: $name"

# ---- base -------------------------------------------------------------------

if [ "$do_fetch" = true ] && agent_have git; then
    git fetch --quiet origin master 2>/dev/null || agent_warn "could not fetch origin master (offline?)"
fi

case "$base_kind" in
master) base="$(agent_trunk_ref)" || agent_die "no master ref found; pass --base" ;;
int) base="origin/int/$area" ;;
*) base="$base_kind" ;;
esac
git rev-parse --verify --quiet "$base^{commit}" >/dev/null || agent_die "base ref '$base' does not exist"

# ---- existing state ---------------------------------------------------------

if git rev-parse --verify --quiet "refs/heads/$name" >/dev/null; then
    agent_die "local branch $name already exists (use: git worktree list)"
fi
if git ls-remote --exit-code --heads origin "$name" >/dev/null 2>&1; then
    agent_die "branch $name already exists on origin"
fi

worktree="$(agent_main_root)/.worktrees/$name"
if [ -e "$worktree" ]; then
    agent_die "worktree path already exists: $worktree"
fi

# ---- claim sanity -----------------------------------------------------------

issue_title=""
if agent_have gh && [ "$issue" != "$AGENT_BOOTSTRAP_ISSUE" ]; then
    if json="$(gh issue view "$issue" --json state,title 2>/dev/null)"; then
        state="$(printf '%s' "$json" | tr -d '\n' | sed -n 's/.*"state":"\([A-Z]*\)".*/\1/p')"
        issue_title="$(printf '%s' "$json" | sed -n 's/.*"title":"\([^"]*\)".*/\1/p')"
        [ "$state" = "OPEN" ] || agent_die "issue #$issue is $state; reopen it or pick another"
    else
        agent_warn "could not read issue #$issue (offline or no permission); continuing"
    fi
elif [ "$issue" = "$AGENT_BOOTSTRAP_ISSUE" ]; then
    agent_log "no issue: the PR is the record; open one from the PR if this outlives a single change"
fi

# ---- create -----------------------------------------------------------------

if [ "$dry_run" = true ]; then
    agent_log "would create branch   $name"
    agent_log "from                  $base ($(git rev-parse --short "$base"))"
    agent_log "worktree              $worktree"
    agent_log "build dir             $root/build/$slug (created by cmake)"
    exit 0
fi

mkdir -p "$(agent_main_root)/.worktrees"
git worktree add --quiet "$worktree" -b "$name" "$base"
git config "branch.$name.description" "${issue_title:-$name}" 2>/dev/null || true

# Keep .worktrees/ out of `git status` without touching the tracked .gitignore
# on branches that predate it.
exclude="$(git rev-parse --git-path info/exclude)"
if [ -f "$exclude" ] || mkdir -p "$(dirname "$exclude")"; then
    grep -qxF '.worktrees/' "$exclude" 2>/dev/null || printf '.worktrees/\n' >>"$exclude"
fi

preset="$(agent_default_preset)"

cat >&2 <<EOF

branch   $name
worktree $worktree
preset   $preset

next:
  cd "$worktree"
  tools/agents/setup.sh                       # once per clone: hooks + tools
  cmake --preset $preset
  cmake --build --preset $preset --target yuzu yuzu-cmd tests
  tools/agents/claim.sh                       # announce the claim on the issue
  tools/agents/verify.sh                      # the only accepted "it works"
EOF
