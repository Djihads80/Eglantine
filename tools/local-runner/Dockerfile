# Self-hosted GitHub Actions runner that builds Eglantine for Linux (tar.gz + AppImage) and
# Android (APK). Ubuntu 24.04 keeps the Linux binaries on glibc 2.39, like the hosted CI.
FROM ubuntu:24.04

ARG RUNNER_VERSION=2.338.2
ARG QT_VERSION=6.8.3
ARG NDK_VERSION=28.2.13676358
ARG ANDROID_CMAKE_VERSION=3.31.5
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf ca-certificates ccache curl file g++ gcc git glslang-tools jq \
        libasound2-dev libavcodec-dev libavfilter-dev libboost-context-dev libboost-dev \
        libboost-fiber-dev libglu1-mesa-dev libopus-dev libpulse-dev libswscale-dev libtool \
        libudev-dev libusb-1.0-0-dev libva-dev libvdpau-dev libxkbcommon-x11-0 \
        libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 \
        libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 libgl1 libegl1 make \
        mesa-common-dev nasm ninja-build openjdk-17-jdk-headless patch perl pkg-config python3-pip \
        spirv-headers spirv-tools sudo unzip zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# gh, for publishing releases from a job.
RUN curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg \
        -o /usr/share/keyrings/githubcli-archive-keyring.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" \
        > /etc/apt/sources.list.d/github-cli.list \
    && apt-get update && apt-get install -y --no-install-recommends gh \
    && rm -rf /var/lib/apt/lists/*

# Qt from the Qt archive (Ubuntu's 6.4 is older than the hosted CI's), with Qt Charts.
# CMake 3.31 (the source tree needs it; Ubuntu ships 3.28).
RUN pip3 install --break-system-packages --no-cache-dir aqtinstall cmake==3.31.6 \
    && aqt install-qt linux desktop "$QT_VERSION" linux_gcc_64 -m qtcharts -O /opt/qt \
    && rm -rf /root/.cache aqtinstall.log
ENV QT_ROOT_DIR=/opt/qt/${QT_VERSION}/gcc_64

# linuxdeploy and its Qt plugin for the AppImage.
RUN mkdir -p /opt/linuxdeploy && cd /opt/linuxdeploy \
    && curl -fsSLO https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage \
    && curl -fsSLO https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage \
    && chmod +x ./*.AppImage

# Android SDK: the platform, build tools, NDK and CMake the Gradle build asks for.
ENV ANDROID_HOME=/opt/android-sdk ANDROID_SDK_ROOT=/opt/android-sdk
RUN mkdir -p "$ANDROID_HOME/cmdline-tools" && cd /tmp \
    && curl -fsSL -o tools.zip https://dl.google.com/android/repository/commandlinetools-linux-13114758_latest.zip \
    && unzip -q tools.zip && mv cmdline-tools "$ANDROID_HOME/cmdline-tools/latest" && rm tools.zip \
    && yes | "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" --licenses > /dev/null \
    && "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" --install \
        "platforms;android-36" "build-tools;36.0.0" "platform-tools" \
        "ndk;$NDK_VERSION" "cmake;$ANDROID_CMAKE_VERSION" > /dev/null

# The runner refuses to run as root. UID 1000 matches the host user that owns the volumes.
RUN userdel -r ubuntu 2>/dev/null || true \
    && useradd -m -u 1000 -s /bin/bash runner \
    && mkdir -p /home/runner/runner /home/runner/.ccache /home/runner/.gradle /home/runner/.cpm \
    && chown -R runner:runner /home/runner "$ANDROID_HOME"
RUN mkdir -p /opt/actions-runner && cd /opt/actions-runner \
    && curl -fsSL "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz" | tar -xz \
    && ./bin/installdependencies.sh > /dev/null \
    && chown -R runner:runner /opt/actions-runner

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
USER runner
WORKDIR /home/runner
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
