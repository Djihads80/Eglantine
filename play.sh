#!/bin/bash
# Default launch: Eglantine with Mario Kart World on KosmicKrisp (Mesa Vulkan-on-Metal, LunarG SDK),
# sound on. Log: ~/eglantine/play.log.  MoltenVK fallback: play-mvk.sh
CFG="$HOME/.config/eden/sdl2-config.ini"
sed -i '' 's/^audio_muted=true/audio_muted=false/' "$CFG"
SDK="$(ls -d "$HOME"/VulkanSDK/*/macOS | sort -V | tail -1)"
export LIBVULKAN_PATH="$SDK/lib/libvulkan.1.dylib"
export VK_DRIVER_FILES="$SDK/share/vulkan/icd.d/libkosmickrisp_icd.json"
cd "$HOME/eglantine/build/bin" || exit 1
./eden-cli -g "$HOME/Documents/Mario Kart World/work/data/v589824/ExeFS/main" 2>&1 | LC_ALL=C sed "s/\x1b\[[0-9;]*m//g" | tr -d '\000' > "$HOME/eglantine/play.log"
echo "== exited with code ${PIPESTATUS[0]}; log: ~/eglantine/play.log"
grep -a "Driver:" "$HOME/eglantine/play.log" | head -1
grep -a "FPS:" "$HOME/eglantine/play.log" | tail -3
