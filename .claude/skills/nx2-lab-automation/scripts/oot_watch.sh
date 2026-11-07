#!/bin/zsh
# Run the emulator with the in-emulator white-flash watcher for a while. Usage: oot_watch.sh <tag> <seconds> [launch args]
TAG=${1:-watch}; SECS=${2:-90}; shift 2
cd ${REPO:-/home/kraibse/clones/Eglantine}
D=${D:-$HOME/.hermes/cache/scratch/ootlab}
B=$D/watch_$TAG
rm -rf $B; mkdir -p $B
L=(python3 -I tools/nx2/lab_ctl.py -d $D)
D=$D zsh .claude/skills/nx2-lab-automation/scripts/to_gameplay.sh --max-seconds 300 \
    --env EDEN_NX2_FRAME_CAPTURE=$B --env EDEN_NX2_FLASH_WATCH=1 "$@" > /tmp/oot_watch_boot.log 2>&1
sleep 3
for k in ${(s:;:)PRE}; do echo "$k" > $D/keys.txt; sleep $(( ${k#* } / 1000.0 + 0.8 )); done
sleep $SECS
cp $D/emulator.log /tmp/watch_$TAG.emulator.log
$L stop 2>&1 | tail -1
tr -d '\000' < /tmp/watch_$TAG.emulator.log | grep -a "flash watch" | tail -20
echo "saved: $(ls $B/flash-*.tga 2>/dev/null | wc -l) frames"
