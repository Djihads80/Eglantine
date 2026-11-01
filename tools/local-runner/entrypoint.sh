#!/bin/bash
# Runs the GitHub Actions runner from the persistent volume at /home/runner/runner; registers it
# on first start with RUNNER_TOKEN (a registration token, valid for an hour).
set -euo pipefail
dir=/home/runner/runner
if [ ! -x "$dir/run.sh" ]; then
    cp -a /opt/actions-runner/. "$dir/"
fi
cd "$dir"
if [ ! -f .runner ]; then
    : "${RUNNER_URL:?set RUNNER_URL}" "${RUNNER_TOKEN:?set RUNNER_TOKEN for the first start}"
    ./config.sh --unattended --replace --url "$RUNNER_URL" --token "$RUNNER_TOKEN" \
        --name "${RUNNER_NAME:-eglantine-local}" --labels eglantine-local --work _work
fi
exec ./run.sh
