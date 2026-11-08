#!/usr/bin/env bash
# usage: release-notes.sh <output.md>
# Env: KIND TAG SHA REPO BRANCH PREV_TAG MACOS_RUNNER ASSET_DIR (default release-assets).
# The download tables include only assets that exist, so one-platform manual builds are supported.
set -euo pipefail

out="${1:?usage: release-notes.sh <output.md>}"
dir="${ASSET_DIR:-release-assets}"
repo="${REPO:?REPO is required}"
tag="${TAG:?TAG is required}"
sha="${SHA:?SHA is required}"
kind="${KIND:?KIND is required}"
base="https://github.com/${repo}/releases/download/${tag}"
short="${sha:0:7}"
macos_ver="${MACOS_RUNNER:-macos-26}"
macos_ver="${macos_ver#macos-}"

link() { if [[ -e "$dir/$2" ]]; then printf '[%s](%s/%s)' "$1" "$base" "$2"; fi; return 0; }
join() { if [[ -n "$1" && -n "$2" ]]; then printf '%s · %s' "$1" "$2"; else printf '%s%s' "$1" "$2"; fi; }
linux_cell() { join "$(link AppImage "eglantine-$1.AppImage")" "$(link tar.gz "eglantine-$1.tar.gz")"; }
row() { if [[ -n "$2$3" ]]; then printf '| %s | %s | %s | %s |\n' "$1" "$2" "$3" "${4:-}"; fi; }
commits() { git log --no-merges --pretty='- %s (`%h`)' "$@" 2>/dev/null | head -n 100 || true; }

{
  case "$kind" in
    stable|rc)
      core="${tag#v}"
      core="${core%%-rc*}"
      if [[ "$kind" == rc ]]; then
        printf '> **Release candidate of %s.** Please test it and report problems before the final release.\n\n' "$core"
      fi
      section=$(bash tools/ci/changelog-section.sh "$core")
      [[ -n "$(tr -d '[:space:]' <<< "$section")" ]] || {
        echo "CHANGELOG.md has no non-empty [$core] section" >&2
        exit 1
      }
      printf '%s\n\n' "$section"
      ;;
    daily)
      printf 'Automatic daily build of `nx2` at `%s`.\n\n## Changes since the previous daily build\n\n' "$short"
      if [[ -n "${PREV_TAG:-}" ]]; then commits "$PREV_TAG..$sha"; else commits -n 30 "$sha"; fi
      printf '\n'
      ;;
    branch)
      printf 'Test build of branch `%s` at `%s`.\n\n**This is not a release.** It is for testing, and the next push to the branch replaces it.\n\n## Commits on top of nx2\n\n' "${BRANCH:-?}" "$short"
      commits "origin/nx2..$sha"
      printf '\n'
      ;;
    *)
      echo "Unknown kind: $kind" >&2
      exit 1
      ;;
  esac

  x_std=$(linux_cell linux-x86_64)
  x_pgo=$(linux_cell linux-x86_64-pgo)
  a_std=$(linux_cell linux-aarch64)
  a_pgo=$(linux_cell linux-aarch64-pgo)
  if [[ -n "$x_std$x_pgo$a_std$a_pgo" ]]; then
    cat <<'EOF2'
## Linux

The AppImage is self-contained (needs glibc 2.39 or newer: Ubuntu 24.04, Fedora 40, Debian 13, Arch): make it executable with `chmod +x` and run it. The tar.gz uses your system libraries, so Qt 6 and the usual libraries must be installed.

| Build Type | Standard | PGO (Recommended) | Notes |
| --- | --- | --- | --- |
EOF2
    row amd64 "$x_std" "$x_pgo"
    row 'ARM (aarch64)' "$a_std" "$a_pgo"
    printf '\n'
  fi

  w_msvc=$(link 'MSVC zip' eglantine-windows-x86_64.zip)
  w_pgo=$(link 'MinGW PGO zip' eglantine-windows-x86_64-pgo.zip)
  if [[ -n "$w_msvc$w_pgo" ]]; then
    cat <<'EOF2'
## Windows

Windows packages are in-place zip files: extract and run `eglantine.exe`, or `eden-cli.exe` from a terminal.

| Build Type | Standard | PGO (Recommended) | Notes |
| --- | --- | --- | --- |
EOF2
    row 'amd64 / x86_64 (MSVC)' "$w_msvc" '' 'Use if you run into graphical issues on other builds'
    row 'amd64 / x86_64 (MinGW)' '' "$w_pgo" 'Built with MinGW clang; bundles its own DLLs, no Visual C++ redistributable needed'
    printf '\n'
  fi

  m_zip=$(link zip eglantine-macos-arm64.zip)
  m_dmg=$(link dmg eglantine-macos-arm64.dmg)
  if [[ -n "$m_zip$m_dmg" ]]; then
    printf '## macOS\n\n| Build Type | Standard | PGO (Recommended) | Notes |\n| --- | --- | --- | --- |\n'
    row 'arm64 (Apple Silicon)' "$(join "$m_zip" "$m_dmg")" '' "macOS ${macos_ver} or newer. Not notarized: right-click and choose Open the first time, or run `xattr -dr com.apple.quarantine eglantine.app`. `eden-cli` is inside the app, in `Contents/MacOS`."
    printf '\n'
  fi

  if [[ -n "$x_pgo$a_pgo$w_pgo" ]]; then
    echo "The PGO builds are the same programs compiled with profile-guided optimization (clang plus Eglantine's profile); prefer them when your CPU supports it."
  fi
} > "$out"
