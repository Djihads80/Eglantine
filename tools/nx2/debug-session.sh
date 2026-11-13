#!/usr/bin/env bash
# Start Eglantine for an unattended debugging session: the MCP server and the NX2 debug server are
# on, and tools/nx2/session_watch.py records evidence next to it, including after a crash.
#
#   tools/nx2/debug-session.sh [eglantine args...]
#
# Evidence goes to <data>/eglantine/debug-sessions/<timestamp>/ (see POSTMORTEM.md there).
# EGLANTINE_BIN overrides the binary, WATCH_ARGS the watcher options (default: IPC-trace nx2:).
set -u

repo=$(cd "$(dirname "$0")/../.." && pwd)
data=${XDG_DATA_HOME:-$HOME/.local/share}/eglantine
root=$data/debug-sessions
session=$root/$(date +%Y%m%d-%H%M%S)
exe=${EGLANTINE_BIN:-$repo/build/bin/eglantine}

[ -x "$exe" ] || { echo "no binary at $exe" >&2; exit 1; }
mkdir -p "$session"

# The emulator rotates its log on launch: keep the previous run's.
for f in eden_log.txt eden_log.txt.old.txt; do
    [ -f "$data/log/$f" ] && cp -p "$data/log/$f" "$session/prev-$f"
done

# shellcheck disable=SC2086
python3 -I "$repo/tools/nx2/session_watch.py" --out "$session" --binary "$exe" \
    --symbols "$root/symbols" ${WATCH_ARGS---ipc-prefix nx2:} >"$session/watch.log" 2>&1 &
watcher=$!
# Also when the script is interrupted (a frozen emulator killed with Ctrl-C): a leftover watcher
# reconnects to every later emulator's MCP server.
trap 'kill "$watcher" 2>/dev/null' EXIT
echo "session  $session"
echo "watcher  tail -f $session/watch.log"

# A kernel core dump of this process is tens of GB: skip it. crashtrace.so records a backtrace
# of the faulting thread instead (built on first use), and stderr is kept next to the evidence.
ulimit -c 0
preload=$root/crashtrace.so
if [ ! -f "$preload" ] || [ "$repo/tools/nx2/crashtrace.c" -nt "$preload" ]; then
    gcc -shared -fPIC -O1 -o "$preload" "$repo/tools/nx2/crashtrace.c" || preload=
fi
LD_PRELOAD=${preload:+$preload${LD_PRELOAD:+:$LD_PRELOAD}} CRASHTRACE_FILE=$session/crashtrace.txt \
    EDEN_NX2_DEBUG_SERVER=${EDEN_NX2_DEBUG_SERVER:-127.0.0.1:6545} \
    "$exe" --mcp "$@" 2> >(tee "$session/stderr.txt" >&2)
status=$?
echo "emulator exited with status $status" | tee "$session/exit.txt"

# The watcher notices the server is gone after a few seconds, then collects the evidence.
for _ in $(seq 40); do
    [ -f "$session/POSTMORTEM.md" ] && break
    sleep 1
done
kill "$watcher" 2>/dev/null
wait "$watcher" 2>/dev/null

if [ -f "$session/POSTMORTEM.md" ]; then
    echo "post-mortem  $session/POSTMORTEM.md"
    sed -n '/^## Minidump/,/^## Kernel/p' "$session/POSTMORTEM.md" | head -40
fi
exit "$status"
