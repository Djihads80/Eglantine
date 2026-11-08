#!/bin/zsh
# Boot Ocarina of Time hidden and load the newest save. Extra arguments go to `lab_ctl launch`
# (e.g. --tensor, --env NAME=VALUE). Needs $D/qt-copy.ini with keyboard bindings merged in.
REPO=${REPO:-/home/kraibse/clones/Eglantine}
D=${D:-$TMPDIR/ootlab}
GAME="/run/media/kraibse/SOINC/emulation/Switch 2/games/The Legend of Zelda Ocarina of TIme [04004860224DEC000].nup"
cd "$REPO" || exit 1
L=(python3 -I tools/nx2/lab_ctl.py -d "$D")
# Unattended runs must be silent: the null audio engine, muted (see mute_audio.py).
if [ -f "$D/qt-copy.ini" ]; then
  /usr/bin/python3 "$REPO/.claude/skills/nx2-lab-automation/scripts/mute_audio.py" "$D/qt-copy.ini"
fi
$L stop 2>/dev/null
$L launch --hidden --max-seconds 420 --config "$D/qt-copy.ini" \
  --dump-dir ~/Documents/hermes/eglantine/oot-shader-dump --game "$GAME" "$@"
sleep 84; $L press a --hold 200   # title (fully up by ~81 s) -> load screen
sleep 9;  $L press a --hold 200   # newest save
sleep 4;  $L press a --hold 200   # confirm
# Loading is black for ~25-35 s; gameplay is the first bright frame (~145 s from launch).
for i in {1..20}; do
  sleep 4
  luma=$($L capture --settle 1 --out "$D/to_gameplay.png" 2>/dev/null | grep -o '"luma": [0-9.]*' | grep -o '[0-9.]*$')
  [[ -n $luma ]] && (( luma > 0.3 )) && break
done
