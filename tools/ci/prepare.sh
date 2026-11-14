#!/usr/bin/env bash
# Decide what the run is and emit: kind tag title version refspec sha skip prerelease nightly
# publish prev_tag branch. Run from any directory in a full checkout with GH_TOKEN set.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../.." && pwd)
cd "$root"
repo="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is not set}"
out_file="${GITHUB_OUTPUT:-/dev/stdout}"
sha="${BUILD_SHA:-$(git rev-parse HEAD)}"
git cat-file -e "$sha^{commit}"
base=$(tr -d '[:space:]' < VERSION)
mode="${GITHUB_EVENT_NAME:?GITHUB_EVENT_NAME is not set}"
force=false

# Strict SemVer shape; release candidates are represented as X.Y.Z-rcN.
version_re='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-rc([1-9][0-9]*))?$'
[[ "$base" =~ $version_re ]] || {
  echo "VERSION must be X.Y.Z or X.Y.Z-rcN (without leading zeroes): $base" >&2
  exit 1
}
core="${base%%-rc*}"

emit() { printf '%s=%s\n' "$1" "$2" >> "$out_file"; }
oneline() { tr -d '\r\n' <<< "$1"; }
daily_tags() {
  gh release list --repo "$repo" --limit 100 --json tagName |
    jq -r '.[].tagName | select(startswith("daily-"))'
}

# Preserve the manual publish choice even when a manual daily is routed through schedule logic.
if [[ "$mode" == workflow_dispatch ]]; then
  publish="${INPUT_PUBLISH:-true}"
else
  publish=true
fi

daily_branch="${DAILY_BRANCH:-nx2-experimental}"
if [[ "$mode" == workflow_dispatch && "${INPUT_KIND:-branch}" == daily ]]; then
  [[ "${GITHUB_REF_NAME:-}" == "$daily_branch" ]] || {
    echo "A manual daily build can only run from $daily_branch" >&2
    exit 1
  }
  mode=schedule
  force=true
fi

skip=false
prerelease=true
nightly=true
prev_tag=""
branch=""

case "$mode" in
  schedule)
    kind=daily
    day=$(date -u +%Y%m%d)
    tag="daily-$day"
    refspec="$tag"
    title="Eglantine Build $(date -u +%d/%m/%Y)"
    branch="$daily_branch"
    latest=$(daily_tags | head -n1 || true)
    prev_tag=$(daily_tags | grep -vxF "$tag" | head -n1 || true)
    if [[ "$force" == false && -n "$latest" ]] &&
      [[ "$(git rev-list -n1 "$latest" 2>/dev/null || true)" == "$sha" ]]; then
      skip=true
    fi
    ;;

  push)
    tag="v$base"
    refspec="$tag"
    title="Eglantine $tag"
    nightly=false
    if [[ "$base" == *-rc* ]]; then
      kind=rc
    else
      kind=stable
      prerelease=false
    fi
    # Fail before spending runner time if the corresponding changelog section is absent/empty.
    section=$(bash "$here/changelog-section.sh" "$core")
    [[ -n "$(tr -d '[:space:]' <<< "$section")" ]] || {
      echo "CHANGELOG.md has no non-empty [$core] section" >&2
      exit 1
    }
    # Duplicate version tags/releases are a no-op. This does not enforce monotonic SemVer order.
    if git rev-parse -q --verify "refs/tags/$tag" >/dev/null ||
      gh release view "$tag" --repo "$repo" >/dev/null 2>&1; then
      skip=true
    fi
    ;;

  workflow_dispatch|pull_request)
    kind=branch
    if [[ "$mode" == pull_request ]]; then
      branch="${PR_HEAD_REF:?PR_HEAD_REF is not set}"
      pr_title="${PR_TITLE:-}"
    else
      branch="${GITHUB_REF_NAME:?GITHUB_REF_NAME is not set}"
      pr_title=$(gh pr list --repo "$repo" --head "$branch" --state open --json title \
        --jq '.[0].title // ""' 2>/dev/null || true)
    fi
    slug=$(bash "$here/slugify.sh" "$branch")
    tag="branch-$slug"
    refspec="$slug"
    title="Eglantine Branch: ${pr_title:-$branch}"
    ;;

  *)
    echo "Unsupported event: $mode" >&2
    exit 1
    ;;
esac

# What every leg shows its user: "[owner/repo/]branch - <commit time> (<sha>)",
# or "... - <release id>" for a release. The rule lives in display-version.sh so
# the workflows, the tarball, the Nix build and a manual dispatch all agree.
# Two variants, because whether a leg is PGO is a per-leg decision (a dispatch
# can build both), not a property of the run.
owner=${GITHUB_REPOSITORY%%/*}
repo_name=${GITHUB_REPOSITORY##*/}
display_branch=$branch
display_release=''
case "$kind" in
  stable | rc)
    display_branch=${GITHUB_REF_NAME:-}
    display_release=$tag
    ;;
  daily)
    # `branch` carries the daily branch (daily.yml dispatches ci.yml from it);
    # fall back to the configured name for a push-style schedule run.
    display_branch=${branch:-$daily_branch}
    ;;
  branch) ;;
esac
display_args=(--owner "$owner" --repo "$repo_name" --branch "$display_branch" --sha "$sha")
if [[ -n "$display_release" ]]; then
  display_args+=(--release "$display_release")
fi
display=$(bash "$here/display-version.sh" "${display_args[@]}")
display_pgo=$(bash "$here/display-version.sh" "${display_args[@]}" --pgo)
# The Nix side needs the same identity inside a store path name, where slashes
# and parentheses are illegal; the script's --store rendering is the only place
# that rule lives, so the legs do not re-derive it.
display_store=$(bash "$here/display-version.sh" "${display_args[@]}" --store)
display_pgo_store=$(bash "$here/display-version.sh" "${display_args[@]}" --pgo --store)

# The file naming follows the identity: an AppImage, an artifact and a Nix store
# path then carry the same string, so a downloaded file says which build it is.
# `tag` stays a separate, shorter, URL-safe thing: it is the only field a client
# can compare against a published release, and it must not carry slashes.
version=$display_store

title=$(oneline "$title")

emit kind "$kind"
emit tag "$tag"
emit title "$title"
emit version "$version"
emit display "$display"
emit display_pgo "$display_pgo"
emit display_store "$display_store"
emit display_pgo_store "$display_pgo_store"
emit refspec "$refspec"
emit sha "$sha"
emit skip "$skip"
emit prerelease "$prerelease"
emit nightly "$nightly"
emit publish "$publish"
emit prev_tag "$prev_tag"
emit branch "$branch"

{
  echo "### Build scheme"
  echo
  echo "| | |"
  echo "| --- | --- |"
  printf '| kind | `%s` |\n' "$kind"
  printf '| tag | `%s` |\n' "$tag"
  printf '| title | %s |\n' "$title"
  printf '| version | `%s` |\n' "$version"
  printf '| display | `%s` |\n' "$display"
  printf '| display (PGO) | `%s` |\n' "$display_pgo"
  printf '| commit | `%s` |\n' "$sha"
  printf '| skipped | %s |\n' "$skip"
} >> "${GITHUB_STEP_SUMMARY:-/dev/null}"

if [[ "$skip" == true ]]; then
  echo "Skipping $tag: this commit was already published for this release kind." >&2
fi
