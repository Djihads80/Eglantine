#!/usr/bin/env bash
# Fast 32-thread local Linux (x86_64) AppImage + tar.gz builder for WSL2 (Ubuntu 24.04).
# Keeps the CMake build tree and ccache inside WSL2's native ext4 filesystem (~/.cache/eglantine-wsl)
# so incremental builds take ~30-60 seconds on a Ryzen 9 9950X3D, then copies the outputs to /mnt/e/Projects/Eglantine/build/.
set -euo pipefail

REPO_WIN="${1:-/mnt/e/Projects/Eglantine}"
CACHE_ROOT="$HOME/.cache/eglantine-wsl"
BUILD_DIR="$CACHE_ROOT/build-linux"
TOOLS_DIR="$CACHE_ROOT/tools"
STAMP_FILE="$CACHE_ROOT/.deps-installed-v1"

mkdir -p "$CACHE_ROOT" "$BUILD_DIR" "$TOOLS_DIR"

if [[ ! -f "$STAMP_FILE" ]]; then
    echo "[WSL2] Installing Linux build dependencies (one-time setup)..."
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \
        autoconf cmake g++ gcc git glslang-tools libglu1-mesa-dev \
        libpulse-dev libtool libudev-dev libxkbcommon-x11-0 \
        mesa-common-dev nasm ninja-build qt6-base-dev qt6-base-private-dev \
        qt6-charts-dev qt6-tools-dev qt6-wayland libavfilter-dev libavcodec-dev \
        libswscale-dev pkg-config zlib1g-dev libva-dev libvdpau-dev \
        spirv-tools spirv-headers libusb-1.0-0-dev \
        libboost-dev libboost-fiber-dev libboost-context-dev \
        libopus-dev libasound2-dev ccache curl file patchelf desktop-file-utils \
        libxcb-cursor0 libxcb-icccm4 libxcb-image0 \
        libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1
    touch "$STAMP_FILE"
fi

if [[ ! -x "$TOOLS_DIR/linuxdeploy-x86_64.AppImage" ]]; then
    echo "[WSL2] Downloading linuxdeploy + Qt plugin (one-time setup)..."
    curl -fsSL -o "$TOOLS_DIR/linuxdeploy-x86_64.AppImage" \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
    curl -fsSL -o "$TOOLS_DIR/linuxdeploy-plugin-qt-x86_64.AppImage" \
        "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
    chmod +x "$TOOLS_DIR"/*.AppImage
fi

if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    echo "[WSL2] Configuring CMake in $BUILD_DIR..."
    cmake -S "$REPO_WIN" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DYUZU_TESTS=OFF \
        -DYUZU_CMD=ON \
        -DYUZU_ROOM=OFF \
        -DYUZU_ROOM_STANDALONE=OFF \
        -DYUZU_USE_BUNDLED_QT=OFF \
        -DYUZU_USE_BUNDLED_SDL3=ON \
        -DVulkanHeaders_FORCE_BUNDLED=ON \
        -DVulkanUtilityLibraries_FORCE_BUNDLED=ON \
        -DENABLE_QT_TRANSLATION=ON \
        -DENABLE_UPDATE_CHECKER=OFF \
        -DUSE_DISCORD_PRESENCE=ON \
        -DUSE_CCACHE=ON \
        -DNIGHTLY_BUILD=ON
fi

echo "[WSL2] Building eglantine and eden-cli with $(nproc) threads..."
cmake --build "$BUILD_DIR" --target yuzu yuzu-cmd --parallel "$(nproc)"
ccache --show-stats || true

strip "$BUILD_DIR/bin/eglantine" "$BUILD_DIR/bin/eden-cli"

echo "[WSL2] Packaging eglantine-linux-x86_64.tar.gz..."
tar -czf "$REPO_WIN/build/eglantine-linux-x86_64.tar.gz" \
    -C "$BUILD_DIR/bin" eglantine eden-cli

echo "[WSL2] Packaging eglantine-linux-x86_64.AppImage..."
APPDIR="$CACHE_ROOT/AppDir"
rm -rf "$APPDIR"
ICON="$CACHE_ROOT/dev.eglantine.eglantine.png"
cp "$REPO_WIN/dist/Eglantine.png" "$ICON"

export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="$(command -v qmake6 || command -v qmake)"
export EXTRA_QT_MODULES=waylandcompositor
export EXTRA_PLATFORM_PLUGINS='libqwayland-egl.so;libqwayland-generic.so'
export LDAI_VERSION="$(tr -d '[:space:]' < "$REPO_WIN/VERSION")"
export LDAI_OUTPUT="$CACHE_ROOT/eglantine-linux-x86_64.AppImage"
export OUTPUT="$LDAI_OUTPUT"
export ARCH=x86_64

"$TOOLS_DIR/linuxdeploy-x86_64.AppImage" --appdir "$APPDIR" \
    --executable "$BUILD_DIR/bin/eglantine" \
    --executable "$BUILD_DIR/bin/eden-cli" \
    --desktop-file "$REPO_WIN/dist/dev.eglantine.eglantine.desktop" \
    --icon-file "$ICON" \
    --plugin qt \
    --output appimage

cp -f "$LDAI_OUTPUT" "$REPO_WIN/build/eglantine-linux-x86_64.AppImage"
echo "[WSL2] Linux packages ready in $REPO_WIN/build/:"
ls -lh "$REPO_WIN/build/eglantine-linux-x86_64.AppImage" "$REPO_WIN/build/eglantine-linux-x86_64.tar.gz"
