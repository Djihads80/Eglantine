#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The gate. One command, identical on every host and in CI, that decides whether
# a change may be called working. Nothing else counts as evidence.
#
#   tools/agents/verify.sh                # configure, build, test, lint
#   tools/agents/verify.sh --lint-only    # fast: name, headers, spelling, TEMP
#   tools/agents/verify.sh --preset windows-msvc-dev
#
# Compiles are serialized machine-wide through flock: two agents building at
# once look exactly like a flaky build failure.

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tools/agents/lib.sh
. "$here/lib.sh"

usage() {
    cat <<'EOF'
Usage: tools/agents/verify.sh [options]

Options:
  --preset NAME        cmake preset (default: chosen for this host)
  --targets "a b c"    build targets (default: "yuzu yuzu-cmd tests")
  --lint-only          skip configure/build/test
  --no-build           skip configure and build (lint only unless --tests-only)
  --tests-only         run only the suite against an existing build dir
  --no-tests           skip the test stage
  --skip-overlap       do not report branch overlap
  --allow-nonconforming run even though the branch name breaks the grammar
  -h, --help           this text
EOF
}

preset=""
targets=""
build=true
tests=true
overlap=true
lint_only=false
tests_only=false
allow_bad_name=false

while [ $# -gt 0 ]; do
    case "$1" in
    -h | --help)
        usage
        exit 0
        ;;
    --preset)
        preset="${2:-}"
        shift 2
        ;;
    --targets)
        targets="${2:-}"
        shift 2
        ;;
    --lint-only)
        lint_only=true
        shift
        ;;
    --tests-only)
        tests_only=true
        build=false
        shift
        ;;
    --no-build)
        build=false
        shift
        ;;
    --no-tests)
        tests=false
        shift
        ;;
    --skip-overlap)
        overlap=false
        shift
        ;;
    --allow-nonconforming)
        allow_bad_name=true
        shift
        ;;
    -*)
        agent_die "unknown option $1 (try --help)"
        ;;
    *)
        agent_die "unexpected argument $1"
        ;;
    esac
done

root="$(agent_repo_root)"
cd "$root"

fails=0
fail() {
    agent_fail "$*"
    fails=$((fails + 1))
}

# Test stage. The suite is not green on trunk: nine cases fail there and nobody
# has diagnosed them (see tools/agents/known-failures.txt, verified against a
# branch whose only diff from master is tooling). The gate compares against that
# baseline instead of pretending the tree is clean; a *new* failure is what fails.
run_tests() {
    local preset="$1" tests_bin="" junit failing known_fail new_fail stale

    for cand in "build/$preset/bin/tests" "build/$preset/bin/tests.exe"; do
        [ -f "$cand" ] && tests_bin="$cand" && break
    done
    if [ -z "$tests_bin" ]; then
        tests_bin="$(find "build/$preset/bin" -maxdepth 3 -type f \
            \( -name tests -o -name tests.exe \) 2>/dev/null | head -n 1)"
    fi
    if [ -z "$tests_bin" ]; then
        fail "no tests binary under build/$preset/bin (did the build targets include 'tests'?)"
        return
    fi

    junit="$(mktemp)"
    rc=0
    # The suite is known to hang occasionally on trunk (observed once in two
    # runs on this machine), so the gate never waits forever: a timeout is
    # reported as such instead of stalling a CI job.
    timeout "${AGENT_TESTS_TIMEOUT:-900}" "$tests_bin" --reporter junit >"$junit" 2>/dev/null || rc=$?
    if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
        rm -f "$junit"
        fail "tests: timed out after ${AGENT_TESTS_TIMEOUT:-900}s -- the suite hangs intermittently on trunk, see tools/agents/known-failures.txt"
        return
    fi
    failing="$(awk '
        /<testcase /{ if (match($0, / name="[^"]*"/)) { n = substr($0, RSTART+7, RLENGTH-8); f = 0 }; next }
        /<failure/  { if (!f) { print n; f = 1 } }
    ' "$junit" | sort -u)"
    rm -f "$junit"

    known_all="$(sed 's/#.*//; s/[[:space:]]*$//; /^$/d' \
        tools/agents/known-failures.txt 2>/dev/null || true)"
    known_fail="$(printf '%s\n' "$known_all" | sed 's/[[:space:]]*\[flaky\]$//' | sed '/^$/d' | sort -u)"
    known_flaky="$(printf '%s\n' "$known_all" | grep -F '[flaky]' |
        sed 's/[[:space:]]*\[flaky\]$//' | sed '/^$/d' || true)"

    if [ -z "$failing" ]; then
        agent_ok "tests ($tests_bin)"
        [ -n "$known_fail" ] && agent_warn "known-failures.txt is stale: the suite is green, prune it"
        return
    fi

    new_fail="$(comm -23 <(printf '%s\n' "$failing") <(printf '%s\n' "$known_fail") | sed '/^$/d')"
    stale="$(comm -13 <(printf '%s\n' "$failing") <(printf '%s\n' "$known_fail") | sed '/^$/d')"
    # A [flaky] entry passing is expected, not a stale baseline.
    stale="$(printf '%s\n' "$stale" | awk -v flaky="$known_flaky" '
        BEGIN { n = split(flaky, f, "\n"); for (i = 1; i <= n; i++) if (f[i] != "") isf[f[i]] = 1 }
        $0 != "" && !($0 in isf)')"

    if [ -n "$new_fail" ]; then
        fail "tests: new failure(s) outside the baseline"
        printf '%s\n' "$new_fail" | sed 's/^/    /' >&2
    else
        agent_warn "tests: $(printf '%s\n' "$failing" | wc -l | tr -d ' ') known failure(s), unchanged"
    fi
    if [ -n "$stale" ]; then
        agent_warn "known-failures.txt lists cases that now pass; prune:"
        printf '%s\n' "$stale" | sed 's/^/    /' >&2
    fi
}

printf '== gate: %s\n' "$root"
branch="$(agent_current_branch)"
printf '== branch: %s\n' "$branch"

# ---- 1. branch grammar ------------------------------------------------------
# An agent on a hand-named branch cannot produce a result, by construction.
if agent_branch_valid "$branch"; then
    name_area="$(agent_branch_field "$branch" area 2>/dev/null || true)"
    name_issue="$(agent_branch_field "$branch" issue 2>/dev/null || true)"
    if agent_issue_lane_ok "$name_area" "$name_issue"; then
        agent_ok "branch name conforms"
    elif [ "$allow_bad_name" = true ]; then
        agent_warn "branch '$branch' carries no issue in area '$name_area' (allowed by flag)"
    else
        fail "branch '$branch' carries issue 0 in area '$name_area'; behaviour changes need a real issue, only ${AGENT_SELF_DESCRIBING_AREAS// /, } may go without one"
    fi
else
    if [ "$allow_bad_name" = true ]; then
        agent_warn "branch '$branch' breaks the grammar (allowed by flag)"
    else
        fail "branch '$branch' breaks the grammar; create it with tools/agents/branch.sh <issue> <slug> --area <area>"
    fi
fi

trunk="$(agent_trunk_ref)" || trunk=""
if [ -z "$trunk" ]; then
    fail "no master ref; run: git fetch origin master:master"
fi

# ---- 2. configure / build / test -------------------------------------------
if [ "$lint_only" = true ]; then
    agent_skip "configure, build, test (--lint-only)"
elif [ "$build" = true ]; then
    [ -n "$preset" ] || preset="$(agent_default_preset)"
    # The ci-core presets switch the Qt frontend off, so the "yuzu" target does
    # not exist there; asking for it would fail a gate that is otherwise fine.
    if [ -z "$targets" ]; then
        case "$preset" in
        *ci*) targets="yuzu-cmd tests" ;;
        *) targets="yuzu yuzu-cmd tests" ;;
        esac
    fi
    agent_log "preset $preset, targets: $targets"

    lock="$(agent_common_dir)/agent-build.lock"
    exec 9>"$lock"
    if flock -w 7200 9; then
        agent_ok "build lock acquired"
    else
        fail "timed out waiting for the build lock ($lock)"
    fi

    if cmake --preset "$preset" >/tmp/eglantine-configure.log 2>&1; then
        agent_ok "configure ($preset)"
    else
        fail "configure ($preset); see /tmp/eglantine-configure.log"
        tail -n 25 /tmp/eglantine-configure.log >&2 || true
    fi

    if [ "$fails" -eq 0 ]; then
        # -k 0 collects every failure in one pass instead of dying on the first.
        # shellcheck disable=SC2086
        if cmake --build --preset "$preset" -- -k 0 $targets >/tmp/eglantine-build.log 2>&1; then
            agent_ok "build ($targets)"
        else
            fail "build ($targets); see /tmp/eglantine-build.log"
            grep -E 'error:|Error [0-9]|FAILED' /tmp/eglantine-build.log | head -n 25 >&2 || true
        fi
    else
        agent_skip "build (configure failed)"
    fi

    if [ "$tests" = true ] && [ "$fails" -eq 0 ]; then
        run_tests "$preset"
    fi
else
    agent_skip "configure and build (--no-build)"
    if [ "$tests_only" = true ]; then
        [ -n "$preset" ] || preset="$(agent_default_preset)"
        run_tests "$preset"
    fi
fi

if [ "$tests_only" = true ]; then
    printf '\n'
    if [ "$fails" -gt 0 ]; then
        printf 'RESULT: FAIL (%s stage(s))\n' "$fails" >&2
        exit 1
    fi
    printf 'RESULT: PASS\n'
    exit 0
fi

# ---- 3. license headers ----------------------------------------------------
# .ci/license-header.sh diffs against the local master branch by design.
if [ -f .ci/license-header.sh ] && git rev-parse --verify --quiet refs/heads/master >/dev/null; then
    if out="$(sh .ci/license-header.sh 2>&1)"; then
        case "$out" in
        *"All good."*) agent_ok "license headers" ;;
        *) fail "license headers; run .ci/license-header.sh -u and review the diff" ;;
        esac
    else
        fail "license headers (.ci/license-header.sh exited non-zero)"
    fi
else
    agent_skip "license headers (needs a local master ref: git fetch origin master:master)"
fi

# ---- 4. spelling -----------------------------------------------------------
if agent_have codespell; then
    if codespell -q 3 $(git diff --name-only "$trunk...HEAD" 2>/dev/null | grep -E '\.(md|txt|h|cpp|kt|py|sh|yml|yaml)$' || true) >/tmp/eglantine-codespell.log 2>&1; then
        agent_ok "codespell"
    else
        fail "codespell; see /tmp/eglantine-codespell.log"
        head -n 20 /tmp/eglantine-codespell.log >&2
    fi
else
    agent_skip "codespell (not installed: pip install codespell)"
fi

# ---- 5. TEMP markers -------------------------------------------------------
# Debt is visible or it is forgotten. Existing markers are tolerated; the PR
# template asks the author to link the cleanup issue for every new one.
# Source files only: prose that documents the marker form is not a marker.
if [ -n "$trunk" ]; then
    markers=""
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        found="$(git diff "$trunk...HEAD" -- "$f" -U0 2>/dev/null |
            grep -E '^\+.*(//|#|--)\s*TEMP\(' | sed 's/^\+//' || true)"
        [ -n "$found" ] && markers="$markers
  $f: $found"
    done < <(git diff --name-only "$trunk...HEAD" 2>/dev/null |
        grep -E '\.(c|cc|cpp|h|hpp|hxx|cxx|kt|kts|qml|py|sh)$' || true)

    if [ -n "$markers" ]; then
        agent_warn "this branch adds TEMP() markers -- each needs a cleanup issue:$markers"
    else
        agent_ok "no new TEMP() markers in source files"
    fi
fi

# ---- 6. overlap report -----------------------------------------------------
if [ "$overlap" = true ] && agent_have gh && [ -n "$branch" ] && [ "$branch" != HEAD ]; then
    mine="$(git diff --name-only "$trunk...HEAD" 2>/dev/null || true)"
    hits=0
    for pr in $(gh pr list --state open --json number --jq '.[].number' 2>/dev/null || true); do
        theirs="$(gh pr diff "$pr" --name-only 2>/dev/null || true)"
        shared="$(comm -12 <(printf '%s\n' "$mine" | sort -u) <(printf '%s\n' "$theirs" | sort -u) | grep -v '^$' || true)"
        if [ -n "$shared" ]; then
            hits=$((hits + 1))
            agent_warn "PR #$pr touches $(printf '%s\n' "$shared" | wc -l | tr -d ' ') of your files"
            printf '%s\n' "$shared" | sed 's/^/    /' >&2
        fi
    done
    [ "$hits" -eq 0 ] && agent_ok "no open PR overlaps"
fi

# ---- verdict ---------------------------------------------------------------
printf '\n'
if [ "$fails" -gt 0 ]; then
    printf 'RESULT: FAIL (%s)\n' "$fails stage(s)" >&2
    exit 1
fi
printf 'RESULT: PASS\n'
