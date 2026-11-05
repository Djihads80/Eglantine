#!/bin/zsh
# Boot Ocarina of Time hidden and load the newest save. Extra arguments go to `lab_ctl launch`
# (e.g. --tensor, --env NAME=VALUE). Needs $D/qt-copy.ini with keyboard bindings merged in.
REPO=${REPO:-/home/kraibse/clones/Eglantine}
D=${D:-$TMPDIR/ootlab}
GAME="/run/media/kraibse/SOINC/emulation/Switch 2/games/The Legend of Zelda Ocarina of TIme [04004860224DEC000].nup"
cd "$REPO" || exit 1
L=(python3 -I tools/nx2/lab_ctl.py -d "$D")
$L stop 2>/dev/null
$L launch --hidden --max-seconds 420 --config "$D/qt-copy.ini" \
  --dump-dir ~/Documents/hermes/eglantine/oot-shader-dump --game "$GAME" "$@"
sleep 90; $L press a --hold 200   # title -> load screen
sleep 10; $L press a --hold 200   # newest save
sleep 4;  $L press a --hold 200   # confirm
sleep 75                          # into gameplay
