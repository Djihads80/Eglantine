#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# One-time per-clone setup: install the hooks, create the worktree root, and
# report which optional gate tools are missing.
#
#   tools/agents/setup.sh

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tools/agents/lib.sh
. "$here/lib.sh"

root="$(agent_repo_root)"
cd "$root"

git config core.hooksPath .githooks
agent_ok "core.hooksPath = .githooks"
git config --local core.whitespace tab-in-indent,trailing-space
agent_ok "core.whitespace = tab-in-indent,trailing-space"

chmod +x .githooks/* 2>/dev/null || true
chmod +x tools/agents/*.sh 2>/dev/null || true
agent_ok "hooks and scripts are executable"

mkdir -p .worktrees
exclude="$(git rev-parse --git-path info/exclude)"
mkdir -p "$(dirname "$exclude")"
grep -qxF '.worktrees/' "$exclude" 2>/dev/null || printf '.worktrees/\n' >>"$exclude"
agent_ok ".worktrees/ excluded from git status"

missing=""
for t in cmake ninja codespell shellcheck clang-format jq gh; do
    agent_have "$t" || missing="$missing $t"
done
if [ -n "$missing" ]; then
    agent_warn "missing optional tools:$missing"
    agent_log "  codespell/shellcheck: pip install codespell shellcheck-py"
    agent_log "  the gate skips a stage it cannot run instead of pretending it passed"
fi

printf '\npreset for this host: %s\n' "$(agent_default_preset)"
cat <<'EOF'

next:
  cmake --preset <preset>
  cmake --build --preset <preset> --target yuzu yuzu-cmd tests
  tools/agents/verify.sh          # the only accepted "it works"
  tools/agents/status.sh          # what is in flight, and on which issue

To start work:
  tools/agents/branch.sh <issue> <slug> --area <area>
EOF
