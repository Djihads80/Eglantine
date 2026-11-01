#!/bin/bash
# play.sh (KosmicKrisp) under lldb; on a crash it prints every thread's backtrace and exits.
# Log: ~/eglantine/play-crash.log
SDK="$(ls -d "$HOME"/VulkanSDK/*/macOS | sort -V | tail -1)"
export LIBVULKAN_PATH="$SDK/lib/libvulkan.1.dylib"
export VK_DRIVER_FILES="$SDK/share/vulkan/icd.d/libkosmickrisp_icd.json"
cd "$HOME/eglantine/build/bin" || exit 1
lldb --batch -o run -k "thread backtrace all" -k "process kill" -k quit -- ./eden-cli -g "$HOME/Documents/Mario Kart World/work/data/v589824/ExeFS/main" > "$HOME/eglantine/play-crash.log" 2>&1
echo "== done; log: ~/eglantine/play-crash.log"
grep -a 'stop reason\|frame #' "$HOME/eglantine/play-crash.log" | head -30 | cut -c1-160
