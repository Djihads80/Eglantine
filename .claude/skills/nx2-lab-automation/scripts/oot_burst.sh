#!/bin/zsh
# Load the save, leave the camera alone, save every presented frame for a while. Usage: oot_burst.sh <tag> <frames> [launch args]
TAG=$1; N=$2; shift 2
cd ${REPO:-/home/kraibse/clones/Eglantine}
D=${D:-$TMPDIR/ootlab}
B=$D/burst_$TAG
rm -rf $B; mkdir -p $B
L=(python3 -I tools/nx2/lab_ctl.py -d $D)
D=$D zsh .claude/skills/nx2-lab-automation/scripts/to_gameplay.sh --max-seconds 300 --env EDEN_NX2_FRAME_CAPTURE=$B "$@" > /tmp/oot_burst_boot.log 2>&1
sleep 5
# PRE="<scancode> <ms>;..." turns the camera/walks before the burst (I/J/K/L = 12/13/14/15 camera).
for k in ${(s:;:)PRE}; do echo "$k" > $D/keys.txt; sleep $(( ${k#* } / 1000.0 + 0.8 )); done
[ -n "$PRE" ] && $L capture --settle 2 --out $D/burst_${TAG}_view.png > /dev/null 2>&1
rm -f $B/nx2-frame-*.tga
echo $N > $B/burst
start=$(date +%s)
while [ -f $B/burst ] && [ $(( $(date +%s) - start )) -lt 10 ]; do sleep 0.5; done
first=$(ls $B | head -1)
sleep $(( N / 20 + 5 ))
cp $D/emulator.log /tmp/burst_$TAG.emulator.log
$L stop 2>&1 | tail -1
echo "frames: $(ls $B/nx2-frame-*.tga 2>/dev/null | wc -l)"
