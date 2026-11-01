#!/bin/bash
# Uploads files to the release for one OS family of the current commit, creating it first if no
# build has published to it yet. Local and hosted runs share these releases: tag <sha10>-<os>.
#   publish.sh <linux|windows|android|macos> <files...>
set -euo pipefail
os=$1; shift
sha=${GITHUB_SHA:-$(git rev-parse HEAD)}
tag="${sha::10}-$os"
repo=${GITHUB_REPOSITORY:-jojo-chaechae/Eglantine}
case "$os" in
linux) notes="AppImage: make it executable (chmod +x) and run it; it bundles its libraries and needs glibc 2.39 or newer (Ubuntu 24.04, Fedora 40, Debian 13, Arch).
tar.gz: extract it and run ./eglantine or ./eden-cli; it uses your system libraries, so Qt 6 and the usual libraries must be installed.
The -pgo builds are the same programs compiled with clang and Eden's profile; prefer them." ;;
windows) notes="Extract the zip and run eglantine.exe, or eden-cli.exe from a terminal. The builds carry their own DLLs. The -pgo builds are compiled with clang and Eden's profile; prefer them." ;;
android) notes="arm64 phones: install the apk (allow installs from unknown sources)." ;;
macos) notes="Apple Silicon, macOS 15 or newer. Unzip and move eglantine.app to Applications; eden-cli is in Contents/MacOS. The app is not notarized: the first time, right-click it and choose Open (or run xattr -dr com.apple.quarantine eglantine.app)." ;;
*) notes="" ;;
esac
if ! gh release view "$tag" --repo "$repo" > /dev/null 2>&1; then
    gh release create "$tag" --repo "$repo" --target "$sha" --prerelease \
        --title "Eglantine ${sha::10} ($os)" \
        --notes "Eglantine $os build of ${sha}.

$notes" ||
        gh release view "$tag" --repo "$repo" > /dev/null # created by a parallel job meanwhile
fi
gh release upload "$tag" "$@" --repo "$repo" --clobber
echo "https://github.com/$repo/releases/tag/$tag"
