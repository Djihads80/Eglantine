#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Shared definitions for tools/agents/*.
#
# Every script, git hook and CI job validates branch names through this file, so
# the grammar lives in exactly one place. Sourcing it writes nothing.

set -euo pipefail

# Areas. Must stay in sync with docs/agents/branching.md and the issue forms.
AGENT_AREAS="${AGENT_AREAS:-vulkan shader core hle android qt audio input network cmake ci docs meta}"
AGENT_AI_TOOLS="${AGENT_AI_TOOLS:-claude codex amp opencode gemini copilot other}"

# Issue field: <issue>-<slug>. Issue 0 means "no issue: the PR is the record".
# It is legal only in a self-describing area, where the change is decidable from
# the diff alone. A behaviour change carries a real issue, because the reason for
# it is not in the diff. AGENT_BOOTSTRAP_ISSUE keeps the historical name for 0.
AGENT_BOOTSTRAP_ISSUE=0
AGENT_SELF_DESCRIBING_AREAS="${AGENT_SELF_DESCRIBING_AREAS:-cmake ci docs meta}"

# agent_issue_lane_ok <area> <issue>
# May this area carry no issue? Non-zero issues are always fine; 0 only in a
# self-describing area. Callers that pass "-" (int/, sync/, rel/) are unaffected.
agent_issue_lane_ok() {
    local area="${1:-}" issue="${2:-}"
    [ -n "$issue" ] || return 1
    [ "$issue" != "$AGENT_BOOTSTRAP_ISSUE" ] && return 0
    case " $AGENT_SELF_DESCRIBING_AREAS " in
    *" $area "*) return 0 ;;
    esac
    return 1
}

agent_log() { printf '  %s\n' "$*" >&2; }
agent_ok() { printf 'ok    %s\n' "$*"; }
agent_skip() { printf 'skip  %s\n' "$*"; }
agent_warn() { printf 'warn  %s\n' "$*" >&2; }
agent_fail() { printf 'FAIL  %s\n' "$*" >&2; }
agent_die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

# One POSIX ERE per branch kind. A name must match exactly one.
#
# A segment is [a-z0-9]+(-[a-z0-9]+)* so that a leading, trailing or doubled
# dash is rejected here as well as in branch.sh.
agent_branch_regex() {
    local areas tools seg
    areas="$(printf '%s' "$AGENT_AREAS" | tr ' ' '|')"
    tools="$(printf '%s' "$AGENT_AI_TOOLS" | tr ' ' '|')"
    seg='[a-z0-9]+(-[a-z0-9]+)*'
    printf '%s\n' \
        "^work/${seg}/(${areas})/[0-9]+-${seg}\$" \
        "^exp/${seg}/(${areas})/[0-9]+-${seg}\$" \
        "^ai/(${tools})/[0-9]+-${seg}\$" \
        "^int/(${areas})\$" \
        "^sync/upstream-[0-9]{8}\$" \
        "^rel/[0-9]+\.[0-9]+\.[0-9]+\$"
}

agent_branch_valid() {
    local name="${1:-}" re
    [ -n "$name" ] || return 1
    while IFS= read -r re; do
        [[ "$name" =~ $re ]] && return 0
    done < <(agent_branch_regex)
    return 1
}

# Print "kind owner area issue slug" for a conforming name; fail otherwise.
# Absent fields are printed as "-" so the output is always five columns.
agent_branch_parse() {
    local name="${1:-}"
    agent_branch_valid "$name" || return 1

    case "$name" in
    work/* | exp/*)
        local kind rest owner area tail
        kind="${name%%/*}"
        rest="${name#*/}"
        owner="${rest%%/*}"
        rest="${rest#*/}"
        area="${rest%%/*}"
        tail="${rest#*/}"
        printf '%s %s %s %s %s\n' "$kind" "$owner" "$area" "${tail%%-*}" "${tail#*-}"
        ;;
    ai/*)
        local tool tail
        tool="${name#ai/}"
        tool="${tool%%/*}"
        tail="${name##*/}"
        printf 'ai %s - %s %s\n' "$tool" "${tail%%-*}" "${tail#*-}"
        ;;
    int/*)
        printf 'int - %s - -\n' "${name#int/}"
        ;;
    sync/*)
        printf 'sync - - - %s\n' "${name#sync/}"
        ;;
    rel/*)
        printf 'rel - - - %s\n' "${name#rel/}"
        ;;
    esac
}

agent_branch_field() {
    # agent_branch_field <name> <kind|owner|area|issue|slug>
    local name="$1" field="$2" parsed
    parsed="$(agent_branch_parse "$name")" || return 1
    # shellcheck disable=SC2086
    set -- $parsed
    case "$field" in
    kind) printf '%s\n' "$1" ;;
    owner) printf '%s\n' "$2" ;;
    area) printf '%s\n' "$3" ;;
    issue) printf '%s\n' "$4" ;;
    slug) printf '%s\n' "$5" ;;
    *) return 1 ;;
    esac
}

agent_repo_root() {
    git rev-parse --show-toplevel 2>/dev/null || agent_die "not inside a git work tree"
}

# The shared git directory. In a linked worktree $root/.git is a file, so every
# path that must be shared (locks, info/exclude) is resolved from here.
agent_common_dir() {
    local p
    p="$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null)" ||
        p="$(git rev-parse --git-common-dir 2>/dev/null)" ||
        agent_die "not inside a git work tree"
    printf '%s\n' "$p"
}

# The main checkout, even when called from a linked worktree. New worktrees are
# always created next to the main one, never nested inside another worktree.
agent_main_root() {
    local common
    common="$(agent_common_dir)"
    case "$common" in
    *.git) printf '%s\n' "${common%/.git}" ;;
    *) printf '%s\n' "$(dirname "$common")" ;;
    esac
}

# The branch this checkout is on, or "HEAD" when detached.
agent_current_branch() {
    git symbolic-ref --quiet --short HEAD 2>/dev/null || printf 'HEAD\n'
}

# Remote that carries trunk. Origin in this repo; upstream is added by hand.
agent_trunk_ref() {
    local ref
    for ref in origin/master master; do
        git rev-parse --verify --quiet "refs/remotes/$ref" >/dev/null 2>&1 && {
            printf '%s\n' "$ref"
            return 0
        }
        git rev-parse --verify --quiet "refs/heads/$ref" >/dev/null 2>&1 && {
            printf '%s\n' "$ref"
            return 0
        }
    done
    return 1
}

# Pick the preset matching this host. GCC unless CC already names clang.
agent_default_preset() {
    case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN* | Windows_NT)
        printf 'windows-msvc-dev\n'
        ;;
    *)
        if printf '%s' "${CC:-}" | grep -qi clang; then
            printf 'linux-clang-dev\n'
        else
            printf 'linux-gcc-dev\n'
        fi
        ;;
    esac
}

agent_have() { command -v "$1" >/dev/null 2>&1; }

# GitHub login of the current operator, with a deterministic fallback so the
# scripts still work offline.
agent_owner_login() {
    local login
    if agent_have gh; then
        login="$(gh api user --jq .login 2>/dev/null || true)"
        [ -n "$login" ] && {
            printf '%s\n' "$login"
            return 0
        }
    fi
    login="$(git config user.github 2>/dev/null || true)"
    [ -n "$login" ] || login="$(git config user.name 2>/dev/null || true)"
    [ -n "$login" ] || agent_die "cannot determine owner login; pass --owner"
    printf '%s\n' "$(printf '%s' "$login" | tr '[:upper:] ' '[:lower:]-' | tr -cd 'a-z0-9-')"
}
