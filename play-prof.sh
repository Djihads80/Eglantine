#!/bin/bash
# play.sh (MoltenVK) with the GPU-thread profiler on: every 10 s the log gets a per-zone breakdown.
# Log: ~/eglantine/play-prof.log
CFG="$HOME/.config/eden/sdl2-config.ini"
sed -i '' 's/^audio_muted=true/audio_muted=false/' "$CFG"
cd "$HOME/eglantine/build/bin" || exit 1
export LIBVULKAN_PATH=/opt/homebrew/lib/libMoltenVK.dylib
export EDEN_NX2_GPU_PROFILE=1
./eden-cli -g "$HOME/Documents/Mario Kart World/work/data/v589824/ExeFS/main" 2>&1 | LC_ALL=C sed "s/\x1b\[[0-9;]*m//g" | tr -d '\000' > "$HOME/eglantine/play-prof.log"
echo "== exited with code ${PIPESTATUS[0]}; log: ~/eglantine/play-prof.log"
grep -a "per second:" "$HOME/eglantine/play-prof.log" | tail -2 | cut -c1-300
