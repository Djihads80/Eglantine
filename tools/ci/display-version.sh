#!/usr/bin/env bash
# Compose the human-facing build identity ("display version").
#
#   <owner>/<repo>/<branch> - <commit time> (<sha7>)     development build
#   <owner>/<repo>/<branch> - <release id>               release build
#
# Prefix rules: the upstream repository prints nothing but the branch; a
# different repository in the organisation prints its name; anything outside the
# organisation prints owner/repo.
#
#   Eglantine-Community/Eglantine     -> nx2-experimental - 2026-11-14_10-35 (ac8ada4)
#   Eglantine-Community/Other         -> Other/weird-branch - 2026-11-14_10-35 (ac8ada4)
#   Octelly/Eglantine                 -> Octelly/Eglantine/nx2 - 2026-11-14_10-35 (ac8ada4)
#   Eglantine-Community/Eglantine tag -> nx2 - v0.1.0-rc3
#   ... --pgo                         -> the same, with " (PGO)" at the end
#
# Nothing here reads the clock. The value is written into the build tree, so a
# build-time timestamp would give every run a new Nix derivation (no binary
# cache) and would make two builds of one commit describe themselves differently.
# The commit's own date is stable, so a rebuilt commit still substitutes.
#
# Usage: display-version.sh --owner O --repo R --branch B --sha S
#                            [--release TAG] [--pgo] [--store]
#                            [--repo-root DIR] [--hash-length N]
#
# --store prints the same identity restricted to [A-Za-z0-9+._?=-], which is
# what a Nix store path name (and therefore a derivation version) accepts.
set -euo pipefail

canonical_owner=Eglantine-Community
canonical_repo=Eglantine

detached_label='[detached HEAD]'
unknown_time='[unknown time]'
unknown_hash='[unknown hash]'
unknown_repo='[unknown repo]'
pgo_display=' (PGO)'
pgo_store='-pgo'

# Nix rejects store path names over 211 bytes and the derivation prefixes
# "eglantine-", so keep the version part well inside that.
store_max=160

date_format='%Y-%m-%d_%H-%M'
hash_len=7
render='display'
release=''
pgo=false
root='.'

repo_slug=${GITHUB_REPOSITORY:-}
owner=${GITHUB_REPOSITORY_OWNER:-}
repo=${repo_slug##*/}
branch=${GITHUB_REF_NAME:-}
sha=${GITHUB_SHA:-}

usage() {
  sed -n '2,29p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while (($#)); do
  case $1 in
    --owner) owner=${2:?--owner needs a value}; shift 2 ;;
    --repo) repo=${2:?--repo needs a value}; shift 2 ;;
    --branch) branch=${2:?--branch needs a value}; shift 2 ;;
    --sha) sha=${2:?--sha needs a value}; shift 2 ;;
    --release) release=${2:?--release needs a value}; shift 2 ;;
    --date-format) date_format=${2:?--date-format needs a value}; shift 2 ;;
    --hash-length) hash_len=${2:?--hash-length needs a value}; shift 2 ;;
    --repo-root) root=${2:?--repo-root needs a value}; shift 2 ;;
    --pgo) pgo=true; shift ;;
    --store) render=store; shift ;;
    -h | --help) usage; exit 0 ;;
    *) echo "display-version.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
done

# Repository prefix. Dropping the repository name only ever happens together
# with dropping the owner: both are omitted for the upstream repository alone.
prefix=''
if [ "$owner" = "$canonical_owner" ]; then
  if [ "$repo" != "$canonical_repo" ]; then
    prefix="${repo:-$unknown_repo}/"
  fi
elif [ -n "$owner" ]; then
  prefix="$owner/${repo:-$unknown_repo}/"
elif [ -n "$repo" ]; then
  prefix="$repo/"
fi

# A detached checkout reports HEAD (or nothing) instead of a branch name.
case $branch in
  '' | HEAD | '(no branch)') branch=$detached_label ;;
esac

# Committer date of the built commit, in UTC. A shallow fetch of exactly that
# commit is enough; if the object is missing, say so rather than invent one.
commit_time=$unknown_time
if [ -n "$sha" ]; then
  if out=$(TZ=UTC git -C "$root" log -1 --date=format-local:"$date_format" --format=%cd "$sha" 2> /dev/null); then
    [ -n "$out" ] && commit_time=$out
  fi
fi

short=${sha:0:$hash_len}
[ -n "$short" ] || short=$unknown_hash

if [ -n "$release" ]; then
  display="${prefix}${branch} - ${release}"
else
  display="${prefix}${branch} - ${commit_time} (${short})"
fi
if [ "$pgo" = true ]; then
  display="${display}${pgo_display}"
fi

if [ "$render" = store ]; then
  # Parentheses, slashes and spaces are not legal in a store path name; the
  # " (PGO)" marker becomes the "-pgo" the rest of the tree already uses.
  stored=${display//"$pgo_display"/$pgo_store}
  stored=$(printf '%s' "$stored" | LC_ALL=C tr -c 'A-Za-z0-9+._?=-' '-')
  stored=$(printf '%s' "$stored" | sed -e 's/-\{2,\}/-/g' -e 's/^[-.]*//' -e 's/-*$//')
  if ((${#stored} > store_max)); then
    stored=${stored:0:$store_max}
    stored=${stored%-}
  fi
  display=${stored:-unknown}
fi

printf '%s\n' "$display"
