// Eglantine Bridge - PC host implementation (protocol v1).
#include "ebridge/host.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#include "ebridge_protocol.h"
#ifdef EBRIDGE_HAVE_VIDEO
#include "video_encoder.h"
#endif
#include <condition_variable>
#ifdef EBRIDGE_HAVE_OPUS
#include <opus.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kBadSocket = INVALID_SOCKET;
static void CloseSocket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kBadSocket = -1;
static void CloseSocket(socket_t s) { close(s); }
#endif

namespace ebridge {
namespace {

using Clock = std::chrono::steady_clock;
using Id = std::array<uint8_t, EB_ID_LEN>;

constexpr auto kSessionTimeout = std::chrono::seconds(5);
constexpr int kPinAttempts = 3;

std::string ToHex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

bool FromHex(const std::string& s, uint8_t* out, size_t n) {
    if (s.size() != n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (std::sscanf(s.c_str() + i * 2, "%2x", &v) != 1) return false;
        out[i] = static_cast<uint8_t>(v);
    }
    return true;
}

void RandomBytes(uint8_t* out, size_t n) {
    std::random_device rd;
    for (size_t i = 0; i < n; i++) out[i] = static_cast<uint8_t>(rd());
}

uint32_t NowUs() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     Clock::now().time_since_epoch())
                                     .count());
}

// Waits until the socket is readable or the timeout passes.
bool WaitReadable(socket_t s, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return select(static_cast<int>(s) + 1, &set, nullptr, nullptr, &tv) > 0;
}

bool RecvExact(socket_t s, void* buf, size_t n) {
    auto* p = static_cast<char*>(buf);
    while (n > 0) {
        const int r = recv(s, p, static_cast<int>(n), 0);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

bool CtlSend(socket_t s, uint16_t type, const void* payload = nullptr, uint16_t len = 0) {
    std::vector<char> buf(sizeof(eb_ctl_header_t) + len);
    eb_ctl_header_t h{type, len};
    std::memcpy(buf.data(), &h, sizeof h);
    if (len) std::memcpy(buf.data() + sizeof h, payload, len);
    return send(s, buf.data(), static_cast<int>(buf.size()), 0) == static_cast<int>(buf.size());
}

bool CtlRecv(socket_t s, uint16_t& type, std::vector<uint8_t>& payload) {
    eb_ctl_header_t h;
    if (!RecvExact(s, &h, sizeof h)) return false;
    payload.resize(h.length);
    if (h.length && !RecvExact(s, payload.data(), h.length)) return false;
    type = h.type;
    return true;
}

float StickToFloat(int16_t v) {
    return v <= -32767 ? -1.0f : static_cast<float>(v) / 32767.0f;
}

} // namespace

struct Host::Impl {
    HostConfig config;
    InputCallback on_input;
    EventCallback on_event;
    GamesProvider games_provider;
    LaunchCallback on_launch;
    std::mutex cb_mutex;

    std::mutex status_mutex;
    eb_ctl_status_t status{};
    uint64_t status_gen = 0;   // +1 per SetStatus; each session sends what it hasn't sent

    Id host_id{};
    std::map<std::string, std::string> paired;  // client id hex -> secret hex
    std::map<std::string, std::string> paired_names;

    socket_t tcp = kBadSocket, udp = kBadSocket, disc = kBadSocket;
    std::atomic<bool> running{false};
    std::thread tcp_thread, udp_thread, disc_thread;

    // Session state, guarded by session_mutex.
    std::mutex session_mutex;
    bool session_active = false;
    uint32_t session_token = 0;
    bool have_udp_addr = false;
    sockaddr_in udp_addr{};
    Clock::time_point last_seen{};
    uint32_t last_input_ts = 0;
    uint32_t rumble_seq = 0;
    uint32_t session_gen = 0;            // +1 per session: reopen the encoder
    eb_ctl_welcome_t stream_params{};    // what the client was promised

    // Video: the render thread drops the newest frame here, the video thread encodes it.
    std::thread video_thread;
    std::mutex video_mutex;
    std::condition_variable video_cv;
    std::vector<uint8_t> pending_frame;
    uint32_t pending_w = 0, pending_h = 0;
    bool has_pending = false;
    std::atomic<bool> idr_requested{false};
    std::atomic<uint32_t> idr_request_count{0};   // for the periodic video stats line

    // Audio: PCM queued until a full 10 ms Opus frame is available.
    std::mutex audio_mutex;
    std::vector<int16_t> audio_pending;
    uint32_t audio_seq = 0, audio_gen = 0;
#ifdef EBRIDGE_HAVE_OPUS
    OpusEncoder* opus = nullptr;
#endif

    void SubmitAudio(const int16_t* pcm, size_t frames) {
#ifdef EBRIDGE_HAVE_OPUS
        uint32_t token, gen;
        sockaddr_in to;
        {
            std::lock_guard lk(session_mutex);
            if (!session_active || !have_udp_addr) return;
            token = session_token;
            gen = session_gen;
            to = udp_addr;
        }
        std::lock_guard lk(audio_mutex);
        if (!opus) {
            int err = 0;
            opus = opus_encoder_create(EB_AUDIO_RATE, EB_AUDIO_CHANNELS, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
            if (!opus) return;
            opus_encoder_ctl(opus, OPUS_SET_BITRATE(128000));
            opus_encoder_ctl(opus, OPUS_SET_COMPLEXITY(5));
        }
        if (gen != audio_gen) {   // new console: fresh encoder state, no stale samples
            audio_gen = gen;
            audio_pending.clear();
            audio_seq = 0;
            opus_encoder_ctl(opus, OPUS_RESET_STATE);
        }
        audio_pending.insert(audio_pending.end(), pcm, pcm + frames * EB_AUDIO_CHANNELS);
        const size_t chunk = EB_AUDIO_FRAME * EB_AUDIO_CHANNELS;
        size_t used = 0;
        uint8_t buf[EB_MAX_UDP_PACKET];
        constexpr size_t kHead = sizeof(eb_udp_header_t) + sizeof(eb_audio_t);
        while (audio_pending.size() - used >= chunk) {
            const int n = opus_encode(opus, audio_pending.data() + used, EB_AUDIO_FRAME, buf + kHead,
                                      static_cast<opus_int32>(sizeof buf - kHead));
            used += chunk;
            if (n <= 0) continue;
            eb_udp_header_t h{EB_UDP_MAGIC, EB_UDP_AUDIO, 0, token, audio_seq, NowUs()};
            eb_audio_t a{audio_seq++, EB_AUDIO_FRAME, EB_AUDIO_CHANNELS, EB_AUDIO_OPUS};
            std::memcpy(buf, &h, sizeof h);
            std::memcpy(buf + sizeof h, &a, sizeof a);
            sendto(udp, reinterpret_cast<const char*>(buf), static_cast<int>(kHead + n), 0,
                   reinterpret_cast<const sockaddr*>(&to), sizeof to);
        }
        audio_pending.erase(audio_pending.begin(), audio_pending.begin() + static_cast<std::ptrdiff_t>(used));
#else
        (void)pcm;
        (void)frames;
#endif
    }

    ~Impl() {
#ifdef EBRIDGE_HAVE_OPUS
        if (opus) opus_encoder_destroy(opus);
#endif
    }

    void Emit(Event e, const std::string& detail) {
        std::lock_guard lk(cb_mutex);
        if (on_event) on_event(e, detail);
    }

    void LoadPaired() {
        std::ifstream f(config.state_file);
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string id, secret, name;
            if (ss >> id >> secret) {
                std::getline(ss, name);
                paired[id] = secret;
                paired_names[id] = name;
            }
        }
    }

    void SavePaired() {
        std::ofstream f(config.state_file, std::ios::trunc);
        for (auto& [id, secret] : paired) f << id << ' ' << secret << paired_names[id] << '\n';
    }

    void DiscoveryLoop() {
        while (running) {
            if (!WaitReadable(disc, 200)) continue;
            uint8_t buf[64];
            sockaddr_in from{};
            socklen_t from_len = sizeof from;
            const int n = recvfrom(disc, reinterpret_cast<char*>(buf), sizeof buf, 0,
                                   reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n < static_cast<int>(sizeof(eb_disc_query_t))) continue;
            eb_disc_query_t q;
            std::memcpy(&q, buf, sizeof q);
            if (q.magic != EB_MAGIC || q.type != EB_DISC_QUERY) continue;

            eb_disc_response_t r{};
            r.magic = EB_MAGIC;
            r.type = EB_DISC_RESPONSE;
            r.version = EB_PROTO_VERSION;
            r.port = config.port;
            std::memcpy(r.host_id, host_id.data(), EB_ID_LEN);
            std::strncpy(r.name, config.name.c_str(), EB_NAME_LEN - 1);
            {
                std::lock_guard lk(session_mutex);
                r.flags = session_active ? EB_HOST_FLAG_BUSY : 0;
            }
            sendto(disc, reinterpret_cast<const char*>(&r), sizeof r, 0,
                   reinterpret_cast<sockaddr*>(&from), from_len);
        }
    }

    void TcpLoop() {
        while (running) {
            if (!WaitReadable(tcp, 200)) continue;
            sockaddr_in from{};
            socklen_t from_len = sizeof from;
            socket_t c = accept(tcp, reinterpret_cast<sockaddr*>(&from), &from_len);
            if (c == kBadSocket) continue;
            int one = 1;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);

            bool busy;
            {
                std::lock_guard lk(session_mutex);
                busy = session_active;
            }
            if (busy) {
                eb_ctl_reject_t rej{EB_REJECT_BUSY, 0};
                CtlSend(c, EB_CTL_REJECT, &rej, sizeof rej);
            } else {
                const std::string reason = HandleClient(c);
                bool was_active;
                {
                    std::lock_guard lk(session_mutex);
                    was_active = session_active;
                    session_active = false;
                    have_udp_addr = false;
                }
                if (was_active) Emit(Event::Disconnected, reason);
            }
            CloseSocket(c);
        }
    }

    // Returns a human readable reason when the client goes away.
    std::string HandleClient(socket_t c) {
        uint16_t type;
        std::vector<uint8_t> payload;
        if (!CtlRecv(c, type, payload) || type != EB_CTL_HELLO ||
            payload.size() != sizeof(eb_ctl_hello_t)) {
            eb_ctl_reject_t rej{EB_REJECT_PROTOCOL, 0};
            CtlSend(c, EB_CTL_REJECT, &rej, sizeof rej);
            return "bad hello";
        }
        eb_ctl_hello_t hello;
        std::memcpy(&hello, payload.data(), sizeof hello);
        if (hello.magic != EB_MAGIC || hello.version != EB_PROTO_VERSION) {
            eb_ctl_reject_t rej{EB_REJECT_BAD_VERSION, 0};
            CtlSend(c, EB_CTL_REJECT, &rej, sizeof rej);
            return "version mismatch";
        }
        hello.name[EB_NAME_LEN - 1] = 0;
        const std::string client_name = hello.name;
        const std::string cid = ToHex(hello.client_id, EB_ID_LEN);
        const std::string secret = ToHex(hello.pair_secret, EB_ID_LEN);

        auto it = paired.find(cid);
        if (it == paired.end() || it->second != secret) {
            if (!Pair(c, cid, client_name)) return "pairing failed";
        }

        eb_ctl_welcome_t w{};
        w.udp_port = config.port;
        w.width = std::min<uint16_t>(config.width, hello.max_width);
        w.height = std::min<uint16_t>(config.height, hello.max_height);
        w.fps = std::min<uint8_t>(config.fps, hello.max_fps ? hello.max_fps : config.fps);
        w.codec = EB_CODEC_H264;
        w.bitrate_kbps = config.bitrate_kbps;
        {
            std::lock_guard lk(session_mutex);
            RandomBytes(reinterpret_cast<uint8_t*>(&session_token), sizeof session_token);
            if (session_token == 0) session_token = 1;
            session_active = true;
            have_udp_addr = false;
            last_seen = Clock::now();
            last_input_ts = 0;
            session_gen++;
            w.session_token = session_token;
            stream_params = w;
        }
        if (!CtlSend(c, EB_CTL_WELCOME, &w, sizeof w)) return "send failed";
        Emit(Event::Connected, client_name);

        std::vector<GameInfo> games;   // the list last sent to this console
        uint64_t status_sent = 0;
        while (running) {
            {
                std::lock_guard lk(session_mutex);
                if (Clock::now() - last_seen > kSessionTimeout) return "timeout";
            }
            {
                eb_ctl_status_t st;
                uint64_t gen;
                {
                    std::lock_guard lk(status_mutex);
                    st = status;
                    gen = status_gen;
                }
                if (gen != status_sent) {
                    if (!CtlSend(c, EB_CTL_STATUS, &st, sizeof st)) return "send failed";
                    status_sent = gen;
                }
            }
            if (!WaitReadable(c, 100)) continue;
            if (!CtlRecv(c, type, payload)) return "connection closed";
            switch (type) {
            case EB_CTL_PING: {
                std::lock_guard lk(session_mutex);   // keeps the session alive in the console's menus
                last_seen = Clock::now();
            }
                CtlSend(c, EB_CTL_PONG, payload.data(), static_cast<uint16_t>(payload.size()));
                break;
            case EB_CTL_GAMES_REQUEST: {
                eb_ctl_games_request_t req{};
                if (payload.size() >= sizeof req) std::memcpy(&req, payload.data(), sizeof req);
                games = ListGames();
                if (!SendGames(c, games, (req.flags & EB_GAMES_WANT_ICONS) != 0)) return "send failed";
                break;
            }
            case EB_CTL_LAUNCH:
                if (payload.size() == sizeof(eb_ctl_launch_t)) {
                    eb_ctl_launch_t l;
                    std::memcpy(&l, payload.data(), sizeof l);
                    if (l.index < games.size()) Launch(games[l.index]);
                }
                break;
            case EB_CTL_BYE:
                return "client quit";
            case EB_CTL_REQUEST_IDR:
                idr_requested = true;
                idr_request_count++;
                break;
            default:
                break;
            }
        }
        return "host stopped";
    }

    std::vector<GameInfo> ListGames() {
        std::lock_guard lk(cb_mutex);
        return games_provider ? games_provider() : std::vector<GameInfo>{};
    }

    void Launch(const GameInfo& g) {
        Emit(Event::Log, "console launches: " + g.name);
        std::lock_guard lk(cb_mutex);
        if (on_launch) on_launch(g);
    }

    static void CopyField(char* dst, size_t cap, const std::string& src) {
        // Cut on a UTF-8 character boundary so the console never sees half a character.
        size_t n = std::min(src.size(), cap - 1);
        while (n > 0 && n < src.size() && (static_cast<uint8_t>(src[n]) & 0xC0) == 0x80) n--;
        std::memcpy(dst, src.data(), n);
    }

    bool SendGames(socket_t c, const std::vector<GameInfo>& games, bool icons) {
        const size_t count = std::min<size_t>(games.size(), 1000);
        for (size_t i = 0; i < count; i++) {
            eb_ctl_game_t e{};
            e.index = static_cast<uint16_t>(i);
            e.flags = games[i].running ? EB_GAME_RUNNING : 0;
            e.title_id = games[i].title_id;
            CopyField(e.name, sizeof e.name, games[i].name);
            CopyField(e.file_type, sizeof e.file_type, games[i].file_type);
            CopyField(e.size, sizeof e.size, games[i].size);
            CopyField(e.play_time, sizeof e.play_time, games[i].play_time);
            if (!CtlSend(c, EB_CTL_GAME_ENTRY, &e, sizeof e)) return false;
        }
        for (size_t i = 0; icons && i < count; i++) {
            const auto& px = games[i].icon_rgb;
            if (px.size() != EB_ICON_BYTES) continue;
            uint8_t buf[sizeof(eb_ctl_game_icon_t) + EB_ICON_CHUNK];
            for (uint32_t off = 0; off < EB_ICON_BYTES; off += EB_ICON_CHUNK) {
                const uint32_t len = std::min<uint32_t>(EB_ICON_CHUNK, EB_ICON_BYTES - off);
                eb_ctl_game_icon_t h{static_cast<uint16_t>(i), EB_ICON_SIZE, off, EB_ICON_BYTES, 0};
                std::memcpy(buf, &h, sizeof h);
                std::memcpy(buf + sizeof h, px.data() + off, len);
                if (!CtlSend(c, EB_CTL_GAME_ICON, buf, static_cast<uint16_t>(sizeof h + len))) return false;
            }
        }
        eb_ctl_games_end_t end{static_cast<uint16_t>(count), 0};
        return CtlSend(c, EB_CTL_GAMES_END, &end, sizeof end);
    }

    bool Pair(socket_t c, const std::string& cid, const std::string& client_name) {
        std::random_device rd;
        const uint32_t pin = rd() % 10000;
        char pin_text[8];
        std::snprintf(pin_text, sizeof pin_text, "%04u", pin);
        Emit(Event::PairingPin, pin_text);

        for (int attempt = 0; attempt < kPinAttempts; attempt++) {
            if (!CtlSend(c, EB_CTL_PAIR_REQUIRED)) return false;
            uint16_t type;
            std::vector<uint8_t> payload;
            // Give the user time to read the PIN and type it on the console.
            if (!WaitReadable(c, 120000) || !CtlRecv(c, type, payload)) return false;
            if (type != EB_CTL_PAIR_PIN || payload.size() != sizeof(eb_ctl_pair_pin_t)) continue;
            eb_ctl_pair_pin_t p;
            std::memcpy(&p, payload.data(), sizeof p);
            if (p.pin != pin) continue;

            eb_ctl_pair_ok_t ok{};
            RandomBytes(ok.pair_secret, EB_ID_LEN);
            std::memcpy(ok.host_id, host_id.data(), EB_ID_LEN);
            paired[cid] = ToHex(ok.pair_secret, EB_ID_LEN);
            paired_names[cid] = " " + client_name;
            SavePaired();
            Emit(Event::Log, "paired with " + client_name);
            return CtlSend(c, EB_CTL_PAIR_OK, &ok, sizeof ok);
        }
        eb_ctl_reject_t rej{EB_REJECT_BAD_PIN, 0};
        CtlSend(c, EB_CTL_REJECT, &rej, sizeof rej);
        return false;
    }

    void UdpLoop() {
        uint8_t buf[2048];
        while (running) {
            if (!WaitReadable(udp, 200)) continue;
            sockaddr_in from{};
            socklen_t from_len = sizeof from;
            const int n = recvfrom(udp, reinterpret_cast<char*>(buf), sizeof buf, 0,
                                   reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n < static_cast<int>(sizeof(eb_udp_header_t))) continue;
            eb_udp_header_t h;
            std::memcpy(&h, buf, sizeof h);
            if (h.magic != EB_UDP_MAGIC) continue;

            uint64_t delta_us = 0;
            {
                std::lock_guard lk(session_mutex);
                if (!session_active || h.session_token != session_token) continue;
                // Learn (or follow) the client's address: handles NAT on remote play.
                udp_addr = from;
                have_udp_addr = true;
                last_seen = Clock::now();
                if (h.type == EB_UDP_INPUT) {
                    delta_us = last_input_ts ? static_cast<uint32_t>(h.timestamp_us - last_input_ts) : 0;
                    last_input_ts = h.timestamp_us;
                }
            }
            if (h.type == EB_UDP_INPUT &&
                n >= static_cast<int>(sizeof(eb_udp_header_t) + sizeof(eb_input_t))) {
                eb_input_t in;
                std::memcpy(&in, buf + sizeof h, sizeof in);
                DeliverInput(in, delta_us);
            }
        }
    }

    void DeliverInput(const eb_input_t& in, uint64_t delta_us) {
        InputState s;
        s.player = in.player;
        s.controller_type = in.controller_type;
        s.buttons = in.buttons & ((1u << kButtonCount) - 1);
        s.lx = StickToFloat(in.lx);
        s.ly = StickToFloat(in.ly);
        s.rx = StickToFloat(in.rx);
        s.ry = StickToFloat(in.ry);
        for (int i = 0; i < 3; i++) {
            s.gyro[i] = in.gyro[i];
            s.accel[i] = in.accel[i];
        }
        s.touch_count = std::min<int>(in.touch_count, kMaxTouch);
        for (int i = 0; i < s.touch_count; i++) {
            s.touch[i].x = in.touch[i].x / 1280.0f;
            s.touch[i].y = in.touch[i].y / 720.0f;
            s.touch[i].id = in.touch[i].id;
        }
        s.delta_us = delta_us;
        std::lock_guard lk(cb_mutex);
        if (on_input) on_input(s);
    }

    void SubmitFrame(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride) {
        {
            std::lock_guard lk(session_mutex);
            if (!session_active || !have_udp_addr) return;
        }
        {
            std::lock_guard lk(video_mutex);
            // Overwrites a frame the encoder has not taken yet: we always send the newest.
            pending_frame.resize(size_t{w} * h * 4);
            for (uint32_t y = 0; y < h; y++)
                std::memcpy(pending_frame.data() + size_t{y} * w * 4, bgra + size_t{y} * stride, w * 4);
            pending_w = w;
            pending_h = h;
            has_pending = true;
        }
        video_cv.notify_one();
    }

    void SendVideoPacket(const uint8_t* data, size_t size, bool key, uint32_t frame_id,
                         uint32_t token, const sockaddr_in& to, uint32_t& seq) {
        const size_t frag_count = (size + EB_VIDEO_FRAG_DATA - 1) / EB_VIDEO_FRAG_DATA;
        if (frag_count == 0 || frag_count > EB_VIDEO_MAX_FRAGS) return;
        uint8_t buf[EB_MAX_UDP_PACKET];
        for (size_t i = 0; i < frag_count; i++) {
            const size_t offset = i * EB_VIDEO_FRAG_DATA;
            const size_t len = std::min<size_t>(EB_VIDEO_FRAG_DATA, size - offset);
            eb_udp_header_t h{EB_UDP_MAGIC, EB_UDP_VIDEO, 0, token, ++seq, NowUs()};
            eb_video_frag_t f{frame_id,
                              static_cast<uint16_t>(i),
                              static_cast<uint16_t>(frag_count),
                              static_cast<uint8_t>(key ? EB_FRAME_KEY : 0),
                              EB_CODEC_H264,
                              0,
                              static_cast<uint32_t>(size)};
            std::memcpy(buf, &h, sizeof h);
            std::memcpy(buf + sizeof h, &f, sizeof f);
            std::memcpy(buf + sizeof h + sizeof f, data + offset, len);
            sendto(udp, reinterpret_cast<const char*>(buf), static_cast<int>(sizeof h + sizeof f + len),
                   0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
        }
    }

    void VideoLoop() {
#ifdef EBRIDGE_HAVE_VIDEO
        VideoEncoder encoder;
        uint32_t open_gen = 0, frame_id = 0, seq = 0;
        std::vector<uint8_t> frame;
        std::vector<EncodedPacket> packets;
        // Stats printed every 5 s while streaming, to diagnose problems on the console side.
        uint32_t st_frames = 0, st_keys = 0, st_packets = 0;
        uint64_t st_bytes = 0;
        double st_encode_ms = 0;
        uint32_t st_encodes = 0;
        auto st_start = Clock::now();
        Clock::time_point last_forced_idr{};
        while (running) {
            uint32_t w, h;
            {
                std::unique_lock lk(video_mutex);
                video_cv.wait_for(lk, std::chrono::milliseconds(200),
                                  [&] { return has_pending || !running; });
                if (!has_pending) continue;
                frame.swap(pending_frame);
                w = pending_w;
                h = pending_h;
                has_pending = false;
            }
            bool active;
            uint32_t token, gen;
            sockaddr_in to;
            eb_ctl_welcome_t params;
            {
                std::lock_guard lk(session_mutex);
                active = session_active && have_udp_addr;
                token = session_token;
                gen = session_gen;
                to = udp_addr;
                params = stream_params;
            }
            if (!active) {
                encoder.Close();
                st_start = Clock::now();
                continue;
            }
            // Keyframes are big: one per 300 ms at most, however often the console asks
            // (while it waits for one it repeats the request every 250 ms).
            bool force_idr = idr_requested.exchange(false);
            if (force_idr && Clock::now() - last_forced_idr < std::chrono::milliseconds(300)) {
                idr_requested = true;
                force_idr = false;
            }
            if (force_idr) last_forced_idr = Clock::now();
            if (gen != open_gen) {
                open_gen = gen;
                std::string chosen, err;
                if (encoder.Open(params.width, params.height, params.fps,
                                 static_cast<int>(params.bitrate_kbps), &chosen, &err)) {
                    Emit(Event::Log, "video encoder: " + chosen + " " + std::to_string(params.width) +
                                         "x" + std::to_string(params.height));
                } else {
                    Emit(Event::Log, "video disabled, no encoder: " + err);
                }
                frame_id = 0;
                force_idr = true;
            }
            if (!encoder.IsOpen()) continue;
            packets.clear();
            const auto t0 = Clock::now();
            if (!encoder.Encode(frame.data(), static_cast<int>(w), static_cast<int>(h),
                                static_cast<int>(w * 4), force_idr, packets))
                continue;
            st_encode_ms += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            st_encodes++;
            for (const auto& p : packets) {
                SendVideoPacket(p.data.data(), p.data.size(), p.key, frame_id++, token, to, seq);
                st_frames++;
                st_keys += p.key ? 1 : 0;
                st_bytes += p.data.size();
                st_packets += static_cast<uint32_t>((p.data.size() + EB_VIDEO_FRAG_DATA - 1) / EB_VIDEO_FRAG_DATA);
            }
            const double secs = std::chrono::duration<double>(Clock::now() - st_start).count();
            if (secs >= 5.0) {
                char line[200];
                std::snprintf(line, sizeof line,
                              "video: %.0f fps, %.0f kbit/s, %u keyframes, %u UDP packets, "
                              "%u keyframes asked by the console, encode %.1f ms",
                              st_frames / secs, st_bytes * 8 / 1000.0 / secs, st_keys, st_packets,
                              idr_request_count.exchange(0),
                              st_encodes ? st_encode_ms / st_encodes : 0.0);
                Emit(Event::Log, line);
                st_frames = st_keys = st_packets = 0;
                st_bytes = 0;
                st_encode_ms = 0;
                st_encodes = 0;
                st_start = Clock::now();
            }
        }
#endif
    }

    bool OpenSockets(std::string* error) {
        auto fail = [&](const char* what) {
            if (error) *error = what;
            return false;
        };
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(config.port);
        int one = 1;

        tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        setsockopt(tcp, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
        if (tcp == kBadSocket || bind(tcp, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
            listen(tcp, 1) != 0)
            return fail("cannot open TCP port (already in use?)");

        udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udp == kBadSocket || bind(udp, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
            return fail("cannot open UDP port (already in use?)");

        if (config.discovery) {
            disc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            setsockopt(disc, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
            addr.sin_port = htons(EB_DISCOVERY_PORT);
            if (disc == kBadSocket || bind(disc, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
                Emit(Event::Log, "discovery port busy, only manual IP:port will work");
                if (disc != kBadSocket) CloseSocket(disc);
                disc = kBadSocket;
            }
        }
        return true;
    }

    void CloseSockets() {
        for (socket_t* s : {&tcp, &udp, &disc}) {
            if (*s != kBadSocket) CloseSocket(*s);
            *s = kBadSocket;
        }
    }
};

Host::Host(HostConfig config) : impl(std::make_unique<Impl>()) {
    impl->config = std::move(config);
}

Host::~Host() { Stop(); }

void Host::SetInputCallback(InputCallback cb) {
    std::lock_guard lk(impl->cb_mutex);
    impl->on_input = std::move(cb);
}

void Host::SetEventCallback(EventCallback cb) {
    std::lock_guard lk(impl->cb_mutex);
    impl->on_event = std::move(cb);
}

void Host::SetStatus(int state, uint32_t value, uint32_t total, const std::string& text,
                     const std::string& game) {
    std::lock_guard lk(impl->status_mutex);
    eb_ctl_status_t s{};
    s.state = static_cast<uint8_t>(state);
    s.value = value;
    s.total = total;
    Impl::CopyField(s.text, sizeof s.text, text);
    Impl::CopyField(s.game, sizeof s.game, game);
    impl->status = s;
    impl->status_gen++;
}

void Host::SetGameCallbacks(GamesProvider list, LaunchCallback launch) {
    std::lock_guard lk(impl->cb_mutex);
    impl->games_provider = std::move(list);
    impl->on_launch = std::move(launch);
}

bool Host::Start(std::string* error) {
    if (impl->running) return true;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (error) *error = "WSAStartup failed";
        return false;
    }
#endif
    // Host id: stable per state file, so consoles recognise this PC.
    impl->LoadPaired();
    const std::string id_key = "host_id";
    if (auto it = impl->paired.find(id_key);
        it == impl->paired.end() || !FromHex(it->second, impl->host_id.data(), EB_ID_LEN)) {
        RandomBytes(impl->host_id.data(), EB_ID_LEN);
        impl->paired[id_key] = ToHex(impl->host_id.data(), EB_ID_LEN);
        impl->SavePaired();
    }
    if (!impl->OpenSockets(error)) {
        impl->CloseSockets();
        return false;
    }
    impl->running = true;
    impl->tcp_thread = std::thread([this] { impl->TcpLoop(); });
    impl->udp_thread = std::thread([this] { impl->UdpLoop(); });
    impl->video_thread = std::thread([this] { impl->VideoLoop(); });
    // Big send buffer: a keyframe is ~100 fragments sent in one burst.
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(impl->udp, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof sndbuf);
    if (impl->disc != kBadSocket) impl->disc_thread = std::thread([this] { impl->DiscoveryLoop(); });
    return true;
}

void Host::Stop() {
    if (!impl->running.exchange(false)) return;
    impl->video_cv.notify_all();
    for (std::thread* t : {&impl->tcp_thread, &impl->udp_thread, &impl->disc_thread, &impl->video_thread})
        if (t->joinable()) t->join();
    impl->CloseSockets();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool Host::IsStreaming() const {
    std::lock_guard lk(impl->session_mutex);
    return impl->session_active && impl->have_udp_addr;
}

void Host::SendRumble(int player, float amp_low, float freq_low, float amp_high, float freq_high) {
    uint8_t buf[sizeof(eb_udp_header_t) + sizeof(eb_rumble_t)];
    sockaddr_in to;
    {
        std::lock_guard lk(impl->session_mutex);
        if (!impl->session_active || !impl->have_udp_addr) return;
        eb_udp_header_t h{EB_UDP_MAGIC, EB_UDP_RUMBLE, 0, impl->session_token, ++impl->rumble_seq,
                          NowUs()};
        std::memcpy(buf, &h, sizeof h);
        to = impl->udp_addr;
    }
    eb_rumble_t r{static_cast<uint8_t>(player), {0, 0, 0}, amp_low, freq_low, amp_high, freq_high};
    std::memcpy(buf + sizeof(eb_udp_header_t), &r, sizeof r);
    sendto(impl->udp, reinterpret_cast<const char*>(buf), sizeof buf, 0,
           reinterpret_cast<sockaddr*>(&to), sizeof to);
}

void Host::SubmitAudio(const int16_t* pcm, size_t frames) { impl->SubmitAudio(pcm, frames); }

void Host::SubmitVideoFrame(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride,
                            uint64_t /*pts_us*/) {
    impl->SubmitFrame(bgra, width, height, stride);
}

} // namespace ebridge
