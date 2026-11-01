#!/bin/bash
# MoltenVK fallback launch. Log: ~/eglantine/play-mvk.log
CFG="$HOME/.config/eden/sdl2-config.ini"
sed -i '' 's/^audio_muted=true/audio_muted=false/' "$CFG"
cd "$HOME/eglantine/build/bin" || exit 1
export LIBVULKAN_PATH=/opt/homebrew/lib/libMoltenVK.dylib MVK_CONFIG_LOG_LEVEL=1
./eden-cli -g "$HOME/Documents/Mario Kart World/work/data/v589824/ExeFS/main" 2>&1 | LC_ALL=C sed "s/\x1b\[[0-9;]*m//g" > "$HOME/eglantine/play-mvk.log"
echo "== exited with code ${PIPESTATUS[0]}; log: ~/eglantine/play-mvk.log"
grep -a "FPS:" "$HOME/eglantine/play-mvk.log" | tail -3
