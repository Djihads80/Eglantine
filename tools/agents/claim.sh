#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Record a claim on an issue and show which other branches already edit the same
# files. The overlap report is the cheap half of collision prevention: it runs
# before the conflict exists, not during a merge.
#
#   tools/agents/claim.sh                  # claim everything changed vs trunk
#   tools/agents/claim.sh path/to/file.cpp
#   tools/agents/claim.sh --dry-run        # print the comment, post nothing

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tools/agents/lib.sh
. "$here/lib.sh"

usage() {
    cat <<'EOF'
Usage: tools/agents/claim.sh [--issue N] [--assign] [--dry-run] [--days N] [paths...]

  With no paths, the claim covers every file this branch changes against trunk.

Options:
  --issue N    issue to comment on (default: parsed from the branch name)
  --assign     also assign the issue to yourself
  --dry-run    print the comment and the overlap table, post nothing
  --days N     how far back to look for active branches (default 60)
EOF
}

issue=""
assign=false
dry_run=false
days=60
paths=()

while [ $# -gt 0 ]; do
    case "$1" in
    -h | --help)
        usage
        exit 0
        ;;
    --issue)
        issue="${2:-}"
        shift 2
        ;;
    --assign)
        assign=true
        shift
        ;;
    --dry-run)
        dry_run=true
        shift
        ;;
    --days)
        days="${2:-60}"
        shift 2
        ;;
    -*)
        agent_die "unknown option $1 (try --help)"
        ;;
    *)
        paths+=("$1")
        shift
        ;;
    esac
done

root="$(agent_repo_root)"
cd "$root"

branch="$(agent_current_branch)"
trunk="$(agent_trunk_ref)" || agent_die "no master ref; cannot compute the claim"

if [ -z "$issue" ]; then
    issue="$(agent_branch_field "$branch" issue 2>/dev/null || true)"
    [ -n "$issue" ] || agent_die "branch '$branch' carries no issue number; pass --issue"
fi

# Issue 0 means "no issue": the PR is the record, so there is nothing to claim on.
if [ "$issue" = "$AGENT_BOOTSTRAP_ISSUE" ]; then
    no_issue=true
else
    no_issue=false
fi

if [ ${#paths[@]} -eq 0 ]; then
    mapfile -t paths < <(git diff --name-only "$trunk...HEAD" 2>/dev/null || true)
fi
if [ ${#paths[@]} -eq 0 ]; then
    agent_warn "no changed files yet; claiming a branch that still touches nothing"
fi

# ---- overlap with other active branches -------------------------------------

cutoff="$(date -d "$days days ago" +%s 2>/dev/null || date -v-"${days}"d +%s)"
report=()
active=0

while read -r ref ts; do
    [ -n "$ref" ] || continue
    other="${ref#refs/remotes/}"
    case "$other" in
    */HEAD | "origin/$branch") continue ;;
    esac
    active=$((active + 1))
    other_files="$(git diff --name-only "$trunk...$ref" 2>/dev/null || true)"
    shared=()
    for p in "${paths[@]}"; do
        if printf '%s\n' "$other_files" | grep -qxF -- "$p"; then
            shared+=("$p")
        fi
    done
    if [ ${#shared[@]} -gt 0 ]; then
        report+=("- \`$other\` shares ${#shared[@]} file(s): $(printf '`%s` ' "${shared[@]}")")
    fi
done < <(git for-each-ref --sort=-committerdate \
    --format='%(refname) %(committerdate:unix)' refs/remotes/origin |
    awk -v cutoff="$cutoff" '$2 > cutoff')

if [ ${#report[@]} -gt 0 ]; then
    agent_warn "${#report[@]} of $active active branches touch the same files:"
    printf '    %s\n' "${report[@]}" >&2
else
    agent_ok "no overlap with $active active branches"
fi

if [ "$no_issue" = true ]; then
    agent_log "no issue on this branch: the PR is its record, nothing to claim"
    exit 0
fi

# ---- comment ----------------------------------------------------------------

body="_CLAIM_ $branch"
if [ ${#paths[@]} -gt 0 ]; then
    body="$body

Expected files:
$(printf -- '- `%s`\n' "${paths[@]}")"
fi
if [ ${#report[@]} -gt 0 ]; then
    body="$body

Overlapping active branches:
$(printf '%s\n' "${report[@]}")"
fi

if [ "$dry_run" = true ] || ! agent_have gh; then
    printf '\n--- would comment on #%s ---\n%s\n' "$issue" "$body"
    [ "$dry_run" = true ] || agent_warn "gh not available; nothing posted"
    exit 0
fi

printf '%s\n' "$body" | gh issue comment "$issue" --body-file - >/dev/null &&
    agent_ok "claimed #$issue as $branch"

if [ "$assign" = true ]; then
    gh issue edit "$issue" --add-assignee @me >/dev/null && agent_ok "assigned #$issue to you"
fi
