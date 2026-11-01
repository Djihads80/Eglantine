#!/bin/bash
# Builds and packages one platform inside the runner container, from the checkout in the current
# directory. Results go to ./out as eglantine-<sha>-<platform>.<ext>.
#   build.sh linux-x86_64 | linux-x86_64-pgo | windows-x86_64 | android-arm64 [android app id]
set -euo pipefail
platform=$1
sha=$(git rev-parse HEAD)
name="eglantine-$sha-$platform"
mkdir -p out
llvm=${LLVM_VERSION:-23}

common=(
    -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_TESTING=OFF
    -DYUZU_TESTS=OFF
    -DYUZU_CMD=ON
    -DYUZU_ROOM=OFF
    -DYUZU_ROOM_STANDALONE=OFF
    -DYUZU_USE_BUNDLED_QT=OFF
    -DYUZU_USE_BUNDLED_SDL3=ON
    -DVulkanHeaders_FORCE_BUNDLED=ON
    -DVulkanUtilityLibraries_FORCE_BUNDLED=ON
    -DENABLE_UPDATE_CHECKER=OFF
    -DUSE_DISCORD_PRESENCE=OFF
    -DUSE_CCACHE=ON
)

# Eden's clang profile (https://github.com/Eden-CI/PGO), with the flags Eden's CI uses.
pgo_flags() {
    curl -fsSL -o eden.profdata https://github.com/Eden-CI/PGO/releases/latest/download/eden.profdata
    test -s eden.profdata
    echo "-fprofile-use=$PWD/eden.profdata -Wno-backend-plugin -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date"
}

case "$platform" in
linux-x86_64 | linux-x86_64-pgo)
    flags=("${common[@]}" "-DCMAKE_PREFIX_PATH=$QT_ROOT_DIR")
    if [ "$platform" = linux-x86_64-pgo ]; then
        profile=$(pgo_flags)
        flags+=(
            "-DCMAKE_C_COMPILER=clang-$llvm" "-DCMAKE_CXX_COMPILER=clang++-$llvm"
            "-DCMAKE_EXE_LINKER_FLAGS=-B/usr/lib/llvm-$llvm/bin -fuse-ld=lld"
            "-DCMAKE_C_FLAGS=$profile" "-DCMAKE_CXX_FLAGS=$profile"
        )
    fi
    cmake -S . -B "build/$platform" "${flags[@]}"
    cmake --build "build/$platform" --target yuzu yuzu-cmd --parallel
    bin="build/$platform/bin"
    strip "$bin/eglantine" "$bin/eden-cli"
    tar -czf "out/$name.tar.gz" -C "$bin" eglantine eden-cli
    rm -rf AppDir
    cp dist/Eglantine.png "$RUNNER_TEMP/dev.eglantine.eglantine.png"
    export APPIMAGE_EXTRACT_AND_RUN=1 NO_STRIP=1 ARCH=x86_64 QMAKE="$QT_ROOT_DIR/bin/qmake"
    export LDAI_OUTPUT="out/$name.AppImage" OUTPUT="out/$name.AppImage"
    /opt/linuxdeploy/linuxdeploy-x86_64.AppImage --appdir AppDir \
        --executable "$bin/eglantine" --executable "$bin/eden-cli" \
        --desktop-file dist/dev.eden_emu.eden.desktop \
        --icon-file "$RUNNER_TEMP/dev.eglantine.eglantine.png" --plugin qt
    # AppRun is a symlink at the AppDir root: Qt looks for qt.conf next to it.
    printf '[Paths]\nPrefix = usr\nPlugins = plugins\n' > AppDir/qt.conf
    /opt/linuxdeploy/linuxdeploy-x86_64.AppImage --appdir AppDir --output appimage
    ;;
windows-x86_64)
    # llvm-mingw's clang is the same major as the Linux PGO compiler, so it reads Eden's profile.
    profile=$(pgo_flags)
    export PATH="$LLVM_MINGW/bin:$PATH"
    # Keep the host's pkg-config libraries out of the Windows build, and give OpenSSL's own
    # Configure (which ignores the CMake toolchain) the cross compiler.
    export PKG_CONFIG_LIBDIR=/nonexistent CC=x86_64-w64-mingw32-clang CXX=x86_64-w64-mingw32-clang++ \
        AR=llvm-ar RANLIB=llvm-ranlib RC=x86_64-w64-mingw32-windres
    cmake -S . -B "build/$platform" "${common[@]}" \
        -DCMAKE_TOOLCHAIN_FILE=/opt/eglantine/mingw-x86_64.cmake \
        "-DCMAKE_PREFIX_PATH=$QT_WINDOWS_DIR" "-DQT_HOST_PATH=$QT_ROOT_DIR" \
        -DYUZU_USE_BUNDLED_FFMPEG=ON -DMAKE=/usr/bin/make \
        "-DCMAKE_C_FLAGS=$profile" "-DCMAKE_CXX_FLAGS=-include cstdlib $profile"
    cmake --build "build/$platform" --target yuzu yuzu-cmd --parallel
    unset CC CXX AR RANLIB RC
    /opt/eglantine/package-windows.sh "build/$platform/bin" "$QT_WINDOWS_DIR" \
        "$LLVM_MINGW/x86_64-w64-mingw32" "$RUNNER_TEMP/$name"
    (cd "$RUNNER_TEMP" && zip -qr9 - "$name") > "out/$name.zip"
    ;;
android-arm64)
    app_id=${2:-dev.eglantine.eglantine}
    (cd src/android && ./gradlew --no-daemon "-PappId=$app_id" assembleMainlineRelease)
    apk=$(find src/android/app/build/outputs/apk/mainline/release -name '*.apk' | head -1)
    cp "$apk" "out/$name.apk"
    ;;
*)
    echo "unknown platform $platform" >&2
    exit 1
    ;;
esac
ls -la out
