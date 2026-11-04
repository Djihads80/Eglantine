#!/bin/bash
# Builds the runner image and (re)starts the container, registering it with the repository
# through the gh CLI's login. Caches live in named volumes and survive rebuilds.
#   tools/local-runner/setup.sh [owner/repo]   (default: jojo-chaechae/Eglantine)
set -euo pipefail
repo=${1:-jojo-chaechae/Eglantine}
here=$(cd "$(dirname "$0")" && pwd)
docker build -t eglantine-runner "$here"
docker rm -f eglantine-runner 2>/dev/null || true
token=$(gh api -X POST "repos/$repo/actions/runners/registration-token" -q .token)
for volume in eglantine-runner eglantine-ccache eglantine-gradle eglantine-cpm; do
    docker volume create "$volume" > /dev/null
done
docker run -d --name eglantine-runner --restart unless-stopped \
    -e RUNNER_URL="https://github.com/$repo" -e RUNNER_TOKEN="$token" \
    -e CCACHE_DIR=/home/runner/.ccache -e CPM_SOURCE_CACHE=/home/runner/.cpm \
    -v eglantine-runner:/home/runner/runner \
    -v eglantine-ccache:/home/runner/.ccache \
    -v eglantine-gradle:/home/runner/.gradle \
    -v eglantine-cpm:/home/runner/.cpm \
    eglantine-runner > /dev/null
echo "eglantine-runner started for $repo"
