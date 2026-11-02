#!/bin/bash
# Launches the Eglantine GUI (build-qt) on KosmicKrisp. Log: ~/eglantine/eglantine-gui.log
# MoltenVK instead: EGL_DRIVER=mvk bash eglantine.sh
SDK="$(ls -d "$HOME"/VulkanSDK/*/macOS | sort -V | tail -1)"
export LIBVULKAN_PATH="$SDK/lib/libvulkan.1.dylib"
if [ "${EGL_DRIVER:-kk}" = "mvk" ]; then
  export VK_DRIVER_FILES="$SDK/share/vulkan/icd.d/MoltenVK_icd.json"
else
  export VK_DRIVER_FILES="$SDK/share/vulkan/icd.d/libkosmickrisp_icd.json"
fi
APP="$(ls -d "$HOME"/eglantine/build-qt/bin/*.app 2>/dev/null | head -1)"
BIN="$(ls "$APP"/Contents/MacOS/* 2>/dev/null | head -1)"
[ -x "$BIN" ] || { echo "GUI binary not found under build-qt/bin — did the build finish?"; exit 1; }
"$BIN" "$@" 2>&1 | LC_ALL=C sed "s/\x1b\[[0-9;]*m//g" | tr -d '\000' > "$HOME/eglantine/eglantine-gui.log"
echo "== exited with code ${PIPESTATUS[0]}; log: ~/eglantine/eglantine-gui.log"
