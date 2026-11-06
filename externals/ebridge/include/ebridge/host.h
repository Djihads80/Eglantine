// Eglantine Bridge - PC host library.
// Discovery, pairing, input in; video, audio and rumble out.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ebridge {

constexpr int kButtonCount = 20;   // matches VirtualGamepad::VirtualButton
constexpr int kMaxTouch = 4;

struct TouchPoint {
    float x = 0, y = 0;   // normalized 0..1 on the guest screen
    int id = 0;
};

struct InputState {
    int player = 0;
    int controller_type = 0;      // EB_CTRL_*
    uint32_t buttons = 0;         // bit N == VirtualButton N
    float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1, +y = up
    std::array<float, 3> gyro{};  // revolutions per second
    std::array<float, 3> accel{}; // G
    int touch_count = 0;
    std::array<TouchPoint, kMaxTouch> touch{};
    uint64_t delta_us = 0;        // time since previous packet (sender clock)
};

// A game the console can ask the PC to start.
struct GameInfo {
    std::string name;          // shown on the console
    uint64_t title_id = 0;
    bool running = false;      // the game playing right now
    std::string id;            // opaque for the library (e.g. the file path)
    std::string file_type, size, play_time;   // shown as columns, already formatted
    std::vector<uint8_t> icon_rgb;  // EB_ICON_SIZE^2 RGB888 pixels (64x64), or empty
};

enum class Event {
    PairingPin,      // detail = 4-digit PIN to show to the user
    Connected,       // detail = client name
    Disconnected,    // detail = reason
    Log,             // detail = message
};

struct HostConfig {
    std::string name = "Eglantine";
    uint16_t port = 52141;              // TCP control + UDP stream
    bool discovery = true;              // answer LAN broadcast on 52140
    std::string state_file = "ebridge_paired.txt";  // paired consoles
    uint16_t width = 1280, height = 720;
    uint8_t fps = 60;
    uint32_t bitrate_kbps = 15000;
};

class Host {
public:
    using InputCallback = std::function<void(const InputState&)>;
    using EventCallback = std::function<void(Event, const std::string&)>;
    // Called on a network thread when the console opens its game menu.
    using GamesProvider = std::function<std::vector<GameInfo>()>;
    // Called on a network thread when the console picks a game from that list.
    using LaunchCallback = std::function<void(const GameInfo&)>;

    explicit Host(HostConfig config);
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // Callbacks run on the host's network threads; keep them short.
    void SetInputCallback(InputCallback cb);
    void SetEventCallback(EventCallback cb);
    // Optional: without it the console's game menu shows nothing to start.
    void SetGameCallbacks(GamesProvider list, LaunchCallback launch);

    // What the emulator is doing (EB_STATUS_* from ebridge_protocol.h), shown by the
    // console as a loading screen until EB_STATUS_RUNNING. Thread-safe; the latest
    // value is also sent to a console that connects later.
    void SetStatus(int state, uint32_t value, uint32_t total, const std::string& text,
                   const std::string& game);

    bool Start(std::string* error = nullptr);
    void Stop();

    bool IsStreaming() const;

    // Forward emulator rumble to the console (same values as HidVibrationValue).
    void SendRumble(int player, float amp_low, float freq_low, float amp_high, float freq_high);

    // BGRA frame of any size (scaled to the stream size). Returns at once:
    // encoding runs on its own thread, and an older frame not yet encoded is dropped.
    void SubmitVideoFrame(const uint8_t* bgra, uint32_t width, uint32_t height,
                          uint32_t stride, uint64_t pts_us);

    // 48 kHz stereo interleaved int16, any number of frames per call. Encoded to
    // Opus in 10 ms packets and sent while a console is connected (needs libopus).
    void SubmitAudio(const int16_t* pcm, size_t frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ebridge
