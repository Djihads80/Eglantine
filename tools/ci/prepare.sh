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

# A manual daily is allowed only from nx2 and bypasses the unchanged-commit check.
if [[ "$mode" == workflow_dispatch && "${INPUT_KIND:-branch}" == daily ]]; then
  [[ "${GITHUB_REF_NAME:-}" == nx2 ]] || {
    echo "A manual daily build can only run from nx2" >&2
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
    version="$day-${sha:0:7}"
    title="Eglantine Build $(date -u +%d/%m/%Y)"
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
    version="$tag"
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
    version="$slug-${sha:0:7}"
    title="Eglantine Branch: ${pr_title:-$branch}"
    ;;

  *)
    echo "Unsupported event: $mode" >&2
    exit 1
    ;;
esac

title=$(oneline "$title")

emit kind "$kind"
emit tag "$tag"
emit title "$title"
emit version "$version"
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
  printf '| commit | `%s` |\n' "$sha"
  printf '| skipped | %s |\n' "$skip"
} >> "${GITHUB_STEP_SUMMARY:-/dev/null}"

if [[ "$skip" == true ]]; then
  echo "Skipping $tag: this commit was already published for this release kind." >&2
fi
