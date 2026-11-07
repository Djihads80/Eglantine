#!/usr/bin/env zsh
# Survey a walk: capture a screenshot every few steps to find a view worth burst-testing.
# Usage: oot_survey.sh <tag> [steps]
set -u
TAG=${1:-survey}; STEPS=${2:-8}
cd ${REPO:-/home/kraibse/clones/Eglantine}
D=${D:-$HOME/.hermes/cache/scratch/ootlab}
OUT=$D/survey_$TAG
L=(python3 -I tools/nx2/lab_ctl.py -d $D)
rm -rf $OUT; mkdir -p $OUT
zsh .claude/skills/nx2-lab-automation/scripts/to_gameplay.sh --max-seconds 300 || exit 1
for i in $(seq 1 $STEPS); do
  printf '82 1400\n' > $D/keys.txt
  sleep 2.2
  $L capture --settle 2 --timeout 20 --out $OUT/step_$i.png > /tmp/survey_cap.log 2>&1 ||
    { echo "capture $i failed:"; tail -3 /tmp/survey_cap.log; }
  printf '15 400\n' > $D/keys.txt
  sleep 1.2
  $L capture --settle 2 --timeout 20 --out $OUT/look_$i.png > /tmp/survey_cap.log 2>&1 ||
    { echo "capture look_$i failed:"; tail -3 /tmp/survey_cap.log; }
done
$L stop > /dev/null 2>&1
if ! ls $OUT/*.png > /dev/null 2>&1; then
  echo "no captures: the emulator was not presenting or lab_ctl failed"; exit 1
fi
/usr/bin/python3 - "$OUT" <<'EOF'
import sys, glob, os
from PIL import Image
files = sorted(glob.glob(os.path.join(sys.argv[1], '*.png')), key=lambda p: (p.split('_')[1][0], p))
files = sorted(files, key=lambda p: (int(p.split('_')[-1].split('.')[0]), p.split('/')[-1].split('_')[0]))
cols = 4
rows = (len(files) + cols - 1) // cols
sheet = Image.new('RGB', (cols * 480, rows * 270))
for k, f in enumerate(files):
    sheet.paste(Image.open(f).convert('RGB').resize((480, 270)), ((k % cols) * 480, (k // cols) * 270))
out = os.path.join(os.path.dirname(sys.argv[1]), 'survey_%s.png' % os.path.basename(sys.argv[1]).split('_', 1)[1])
sheet.save(out)
print(out, len(files), 'shots')
EOF
