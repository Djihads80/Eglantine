// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Breakpoints and watchpoints through the emulator's own GDB stub: the MCP connects to it on
// localhost as a GDB client, so the debugger in core is used as it is. The stub only works when
// `use_gdbstub` was on when the game booted (the CPU JIT compiles the debug checks then).

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fmt/format.h>

#include "common/logging.h"
#include "common/settings.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_light_lock.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "mcp/context.h"
#include "mcp/control.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"

namespace Mcp {

namespace {

using namespace std::chrono;

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
void CloseSocket(SocketHandle s) {
    closesocket(s);
}
#else
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
void CloseSocket(SocketHandle s) {
    close(s);
}
#endif

/// Blocking TCP connection to 127.0.0.1 with timed reads.
class Socket {
public:
    ~Socket() {
        Close();
    }

    /// Connects to 127.0.0.1:port. Returns an error message, or "" on success.
    std::string Connect(u16 port) {
#ifdef _WIN32
        static const bool wsa_ready = [] {
            WSADATA data;
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        if (!wsa_ready) {
            return "could not initialize sockets";
        }
#endif
        handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (handle == kNoSocket) {
            return "could not create a socket";
        }
        int yes = 1;
        setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes),
                   sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(handle, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            Close();
            return fmt::format("could not connect to the GDB stub on port {}", port);
        }
        return {};
    }

    void Close() {
        if (handle != kNoSocket) {
            CloseSocket(handle);
            handle = kNoSocket;
        }
    }

    [[nodiscard]] bool IsOpen() const {
        return handle != kNoSocket;
    }

    /// Sends all of data. Returns false (and closes) on failure.
    bool Send(std::string_view data) {
        while (!data.empty() && IsOpen()) {
            const int sent = send(handle, data.data(), static_cast<int>(data.size()), 0);
            if (sent <= 0) {
                Close();
                return false;
            }
            data.remove_prefix(static_cast<size_t>(sent));
        }
        return IsOpen();
    }

    /// Appends whatever arrives within timeout to out. Returns false if the connection closed.
    bool Receive(std::string& out, milliseconds timeout) {
        if (!IsOpen()) {
            return false;
        }
#ifdef _WIN32
        fd_set set;
        FD_ZERO(&set);
        FD_SET(handle, &set);
        timeval tv{static_cast<long>(timeout.count() / 1000),
                   static_cast<long>((timeout.count() % 1000) * 1000)};
        const int ready = select(0, &set, nullptr, nullptr, &tv);
#else
        pollfd pfd{handle, POLLIN, 0};
        const int ready = poll(&pfd, 1, static_cast<int>(timeout.count()));
#endif
        if (ready < 0) {
            Close();
            return false;
        }
        if (ready == 0) {
            return true;
        }
        char buffer[4096];
        const int got = recv(handle, buffer, sizeof(buffer), 0);
        if (got <= 0) {
            Close();
            return false;
        }
        out.append(buffer, static_cast<size_t>(got));
        return true;
    }

private:
    SocketHandle handle = kNoSocket;
};

/// Wraps a payload as a GDB remote protocol packet: $payload#checksum.
std::string FramePacket(std::string_view payload) {
    u8 sum = 0;
    for (const char c : payload) {
        sum = static_cast<u8>(sum + static_cast<u8>(c));
    }
    return fmt::format("${}#{:02x}", payload, sum);
}

/// Removes the first complete packet from buffer and returns its payload. Acks ('+'/'-') are
/// skipped. Returns nullopt while no complete packet has arrived.
std::optional<std::string> TakePacket(std::string& buffer) {
    const size_t start = buffer.find('$');
    if (start == std::string::npos) {
        buffer.clear();
        return std::nullopt;
    }
    const size_t hash = buffer.find('#', start);
    if (hash == std::string::npos || hash + 2 >= buffer.size()) {
        buffer.erase(0, start);
        return std::nullopt;
    }
    std::string payload = buffer.substr(start + 1, hash - start - 1);
    buffer.erase(0, hash + 3);
    // Unescape '}' sequences (the stub escapes '#', '$', '}' and '*').
    std::string out;
    out.reserve(payload.size());
    for (size_t i = 0; i < payload.size(); ++i) {
        if (payload[i] == '}' && i + 1 < payload.size()) {
            out.push_back(static_cast<char>(payload[++i] ^ 0x20));
        } else {
            out.push_back(payload[i]);
        }
    }
    return out;
}

/// Whether a reply is a stop notification (the target halted).
bool IsStopReply(const std::string& packet) {
    return !packet.empty() && (packet[0] == 'T' || packet[0] == 'S');
}

/// Value of `key:` in a stop reply ("thread", "watch", "rwatch", "awatch", or a register number
/// in hex), or nullopt.
std::optional<std::string> StopField(const std::string& packet, std::string_view key) {
    size_t pos = 3; // after "Tss"
    while (pos < packet.size()) {
        const size_t colon = packet.find(':', pos);
        const size_t semi = packet.find(';', pos);
        if (colon == std::string::npos || semi == std::string::npos || colon > semi) {
            return std::nullopt;
        }
        if (std::string_view{packet}.substr(pos, colon - pos) == key) {
            return packet.substr(colon + 1, semi - colon - 1);
        }
        pos = semi + 1;
    }
    return std::nullopt;
}

/// Breakpoint kinds, by GDB Z-packet type.
enum class BreakType { Exec = 0, Write = 2, Read = 3, Access = 4 };

std::string_view BreakTypeName(BreakType type) {
    switch (type) {
    case BreakType::Exec:
        return "exec";
    case BreakType::Write:
        return "write";
    case BreakType::Read:
        return "read";
    case BreakType::Access:
        return "access";
    }
    return "?";
}

struct Breakpoint {
    u32 id = 0;
    std::string expr; ///< As given (e.g. "Module+0x1234").
    u64 address = 0;
    u64 size = 4;
    BreakType type = BreakType::Exec;
    std::string label;
    bool stop = false; ///< Leave the game paused on a hit (otherwise log and continue).
    u64 max_hits = 1000;
    u64 hits = 0;
    bool enabled = true;
};

/// What the target is doing, as far as the client knows.
enum class TargetState { Detached, Running, Paused };

/// Talks to the GDB stub on one worker thread. Tools submit jobs, which run between packets: if
/// the game is running, the worker interrupts it, runs the job and continues (unless the job
/// leaves it paused).
class DebugClient {
public:
    static DebugClient& Get() {
        static DebugClient instance;
        return instance;
    }

    /// Connects to the stub and lets the game run. Returns an error message or "".
    std::string Attach(ToolContext ctx) {
        std::unique_lock lock{mutex};
        if (state != TargetState::Detached) {
            return {};
        }
        if (!ctx.system || !ctx.system->DebuggerEnabled()) {
            return "the GDB stub is not running. Turn it on with settings_set use_gdbstub true, "
                   "then stop and boot the game again (it has to be on at boot).";
        }
        const u16 port = Settings::values.gdbstub_port.GetValue();
        auto sock = std::make_unique<Socket>();
        if (std::string error = sock->Connect(port); !error.empty()) {
            return error;
        }
        context = ctx;
        socket = std::move(sock);
        buffer.clear();
        // The stub pauses the game when a client connects.
        state = TargetState::Paused;
        paused_info = json{{"reason", "attached"}};
        if (!Command("QStartNoAckMode").has_value()) {
            Reset("the GDB stub did not answer (is another debugger attached?)");
            return "the GDB stub did not answer; another debugger may be attached";
        }
        no_ack = true;
        Command("?");
        SendRaw("c");
        state = TargetState::Running;
        paused_info = nullptr;
        worker = std::jthread([this](std::stop_token stop) { Run(stop); });
        Post("debugger attached on port " + std::to_string(port));
        return {};
    }

    /// Removes breakpoints, lets the game run and disconnects.
    void Detach() {
        std::jthread finished;
        {
            std::unique_lock lock{mutex};
            if (state == TargetState::Detached) {
                return;
            }
            Interrupt(lock);
            for (auto& bp : breakpoints) {
                if (bp.enabled) {
                    Command(RemovePacket(bp));
                }
            }
            breakpoints.clear();
            SendRaw("c");
            Reset("debugger detached");
            finished = std::move(worker);
        }
        if (finished.joinable()) {
            finished.request_stop();
            finished.join();
        }
    }

    /// Adds a breakpoint or watchpoint. Returns its id, or 0 with error set.
    u32 Add(Breakpoint bp, std::string& error) {
        std::unique_lock lock{mutex};
        if (state == TargetState::Detached) {
            error = "not attached";
            return 0;
        }
        const bool was_running = state == TargetState::Running;
        Interrupt(lock);
        const auto reply = Command(InsertPacket(bp));
        if (!reply || *reply != "OK") {
            error = fmt::format("the stub refused the {} breakpoint at {:#x} ({})",
                                BreakTypeName(bp.type), bp.address,
                                reply ? *reply : std::string{"no reply"});
            if (bp.type != BreakType::Exec) {
                error += "; hardware watchpoints are limited in number";
            }
        } else {
            bp.id = next_id++;
            breakpoints.push_back(bp);
        }
        if (was_running) {
            Continue();
        }
        return error.empty() ? breakpoints.back().id : 0;
    }

    /// Removes one breakpoint (or all with id 0). Returns how many were removed.
    size_t Remove(u32 id) {
        std::unique_lock lock{mutex};
        if (state == TargetState::Detached) {
            return 0;
        }
        const bool was_running = state == TargetState::Running;
        Interrupt(lock);
        size_t removed = 0;
        for (auto it = breakpoints.begin(); it != breakpoints.end();) {
            if (id == 0 || it->id == id) {
                if (it->enabled) {
                    Command(RemovePacket(*it));
                }
                it = breakpoints.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        if (was_running) {
            Continue();
        }
        return removed;
    }

    /// Resumes a game paused by the debugger. Returns an error message or "".
    std::string Resume() {
        std::unique_lock lock{mutex};
        if (state == TargetState::Detached) {
            return "not attached";
        }
        if (state == TargetState::Running) {
            return {};
        }
        StepOverStoppedBreakpoint();
        Continue();
        return {};
    }

    /// Pauses the game through the debugger. Returns an error message or "".
    std::string Pause() {
        std::unique_lock lock{mutex};
        if (state == TargetState::Detached) {
            return "not attached";
        }
        const bool was_running = state == TargetState::Running;
        Interrupt(lock);
        if (state == TargetState::Detached) {
            return "the GDB stub stopped answering";
        }
        // Keep the stop description (pc, breakpoint): continue and step rely on it.
        if (was_running && paused_info.is_object() && !paused_info.contains("breakpoint")) {
            paused_info["reason"] = "paused by request";
        }
        return {};
    }

    /// Executes one instruction of the paused thread and stays paused. Returns where it stopped.
    json Step(std::string& error) {
        std::unique_lock lock{mutex};
        if (state != TargetState::Paused) {
            error = state == TargetState::Detached ? "not attached"
                                                   : "the game is running; pause it first";
            return nullptr;
        }
        // Stepping off an exec breakpoint needs it removed for that one instruction.
        Breakpoint* bp = StoppedAtExecBreakpoint();
        if (bp) {
            Command(RemovePacket(*bp));
        }
        const auto stop = StepAndWait();
        if (bp) {
            Command(InsertPacket(*bp));
        }
        if (!stop) {
            error = "no stop reply after the step";
            return nullptr;
        }
        paused_info = DescribeStop(*stop, "step");
        return paused_info;
    }

    /// Debugger state for status/dashboard.
    json Status() {
        std::unique_lock lock{mutex};
        json bps = json::array();
        for (const auto& bp : breakpoints) {
            bps.push_back(BreakpointJson(bp));
        }
        return json{
            {"attached", state != TargetState::Detached},
            {"state", state == TargetState::Running  ? "running"
                      : state == TargetState::Paused ? "paused"
                                                     : "detached"},
            {"paused", paused_info},
            {"breakpoints", std::move(bps)},
            {"recent_hits", RecentHitsJson()},
        };
    }

    json RecentHitsJson() const {
        json hits = json::array();
        for (const auto& h : recent_hits) {
            hits.push_back(h);
        }
        return hits;
    }

    [[nodiscard]] bool Attached() {
        std::unique_lock lock{mutex};
        return state != TargetState::Detached;
    }

    ~DebugClient() {
        Detach();
    }

private:
    static json BreakpointJson(const Breakpoint& bp) {
        return json{{"id", bp.id},           {"address", fmt::format("{:#x}", bp.address)},
                    {"expr", bp.expr},       {"type", BreakTypeName(bp.type)},
                    {"size", bp.size},       {"label", bp.label},
                    {"mode", bp.stop ? "stop" : "log"},
                    {"hits", bp.hits},       {"max_hits", bp.max_hits},
                    {"enabled", bp.enabled}};
    }

    static std::string InsertPacket(const Breakpoint& bp) {
        return fmt::format("Z{},{:x},{:x}", static_cast<int>(bp.type), bp.address,
                           bp.type == BreakType::Exec ? 4 : bp.size);
    }

    static std::string RemovePacket(const Breakpoint& bp) {
        return fmt::format("z{},{:x},{:x}", static_cast<int>(bp.type), bp.address,
                           bp.type == BreakType::Exec ? 4 : bp.size);
    }

    /// Sends a raw command packet (no reply expected now, e.g. 'c').
    bool SendRaw(std::string_view payload) {
        return socket && socket->Send(FramePacket(payload));
    }

    /// Reads the next packet within timeout. Stop replies are returned like any other packet.
    std::optional<std::string> ReadPacket(milliseconds timeout) {
        const auto deadline = steady_clock::now() + timeout;
        while (socket && socket->IsOpen()) {
            if (auto packet = TakePacket(buffer)) {
                if (!no_ack) {
                    socket->Send("+");
                }
                return packet;
            }
            const auto left = duration_cast<milliseconds>(deadline - steady_clock::now());
            if (left.count() <= 0) {
                return std::nullopt;
            }
            if (!socket->Receive(buffer, (std::min)(left, milliseconds{100}))) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    /// Sends a command and returns its reply (stop replies arriving meanwhile are handled).
    std::optional<std::string> Command(std::string_view payload) {
        if (!SendRaw(payload)) {
            return std::nullopt;
        }
        for (int i = 0; i < 8; ++i) {
            auto reply = ReadPacket(seconds{5});
            if (!reply) {
                return std::nullopt;
            }
            if (payload != "?" && IsStopReply(*reply)) {
                pending_stops.push_back(*reply);
                continue;
            }
            return reply;
        }
        return std::nullopt;
    }

    /// Stops a running game (Ctrl-C) and waits for the stop reply. Any breakpoint hit that raced
    /// with it is recorded first.
    void Interrupt(std::unique_lock<std::mutex>&) {
        if (state != TargetState::Running) {
            return;
        }
        socket->Send(std::string_view{"\x03", 1});
        auto stop = ReadPacket(seconds{5});
        if (!stop || !IsStopReply(*stop)) {
            Reset("the GDB stub stopped answering");
            return;
        }
        state = TargetState::Paused;
        // A breakpoint hit that raced with the interrupt produces a second stop reply: take both
        // now, so a stale one is not mistaken for a new stop later. Hits among them are recorded.
        std::vector<std::string> stops{*stop};
        while (auto extra = ReadPacket(milliseconds{30})) {
            if (IsStopReply(*extra)) {
                stops.push_back(*extra);
            }
        }
        for (const auto& s : stops) {
            json described = DescribeStop(s, "interrupted");
            if (Breakpoint* bp = MatchStop(described)) {
                RecordHit(*bp, described);
            }
            paused_info = std::move(described);
        }
    }

    void Continue() {
        if (state == TargetState::Detached) {
            return;
        }
        SendRaw("c");
        state = TargetState::Running;
        paused_info = nullptr;
    }

    /// Steps the active thread and returns its stop reply.
    std::optional<std::string> StepAndWait() {
        SendRaw("s");
        for (int i = 0; i < 4; ++i) {
            auto reply = ReadPacket(seconds{5});
            if (!reply) {
                return std::nullopt;
            }
            if (IsStopReply(*reply)) {
                return reply;
            }
        }
        return std::nullopt;
    }

    /// The exec breakpoint the paused thread is sitting on, if any.
    Breakpoint* StoppedAtExecBreakpoint() {
        if (!paused_info.is_object() || !paused_info.contains("pc")) {
            return nullptr;
        }
        const u64 pc = paused_info["pc"].get<u64>();
        for (auto& bp : breakpoints) {
            if (bp.enabled && bp.type == BreakType::Exec && bp.address == pc) {
                return &bp;
            }
        }
        return nullptr;
    }

    /// Before resuming from a breakpoint or watchpoint stop, executes the stopped instruction
    /// with that breakpoint removed, so the game does not stop on it again at once.
    void StepOverStoppedBreakpoint() {
        Breakpoint* bp = nullptr;
        if (paused_info.is_object() && paused_info.contains("breakpoint")) {
            const u32 id = paused_info["breakpoint"].get<u32>();
            for (auto& b : breakpoints) {
                if (b.id == id && b.enabled) {
                    bp = &b;
                }
            }
        }
        if (!bp) {
            return;
        }
        Command(RemovePacket(*bp));
        StepAndWait();
        Command(InsertPacket(*bp));
    }

    /// Thread, registers and symbolized backtrace for a stop reply, read from the stopped thread.
    json DescribeStop(const std::string& packet, std::string reason) {
        json out{{"reason", std::move(reason)}};
        u64 thread_id = 0;
        if (const auto t = StopField(packet, "thread")) {
            thread_id = std::strtoull(t->c_str(), nullptr, 16);
            out["thread"] = thread_id;
        }
        for (const auto key : {"watch", "rwatch", "awatch"}) {
            if (const auto w = StopField(packet, key)) {
                out["data_address"] = std::strtoull(w->c_str(), nullptr, 16);
                out["access"] = key;
            }
        }
        auto* process = context.system ? context.system->ApplicationProcess() : nullptr;
        if (!process) {
            return out;
        }
        Kernel::KScopedLightLock lock{process->GetListLock()};
        for (const auto& thread : process->GetThreadList()) {
            if (thread.GetThreadId() != thread_id) {
                continue;
            }
            const auto& ctx = thread.GetContext();
            out["pc"] = ctx.pc;
            out["thread_name"] = Core::GetThreadName(&thread).value_or("?");
            json regs = json::object();
            for (size_t i = 0; i < 8; ++i) {
                regs[fmt::format("x{}", i)] = fmt::format("{:#x}", ctx.r[i]);
            }
            regs["fp"] = fmt::format("{:#x}", ctx.fp);
            regs["lr"] = fmt::format("{:#x}", ctx.lr);
            regs["sp"] = fmt::format("{:#x}", ctx.sp);
            regs["pc"] = fmt::format("{:#x}", ctx.pc);
            out["registers"] = std::move(regs);
            json frames = json::array();
            for (const auto& f : Core::GetBacktrace(&thread)) {
                frames.push_back(fmt::format("{}+{:#x} {}", f.module, f.offset, f.name));
                if (frames.size() >= 16) {
                    break;
                }
            }
            out["backtrace"] = std::move(frames);
            break;
        }
        return out;
    }

    /// The breakpoint a stop belongs to, or null (e.g. svcBreak or an unknown stop).
    Breakpoint* MatchStop(const json& stop) {
        if (stop.contains("data_address")) {
            const u64 addr = stop["data_address"].get<u64>();
            for (auto& bp : breakpoints) {
                if (bp.enabled && bp.type != BreakType::Exec && addr >= bp.address &&
                    addr < bp.address + bp.size) {
                    return &bp;
                }
            }
        }
        if (stop.contains("pc")) {
            const u64 pc = stop["pc"].get<u64>();
            for (auto& bp : breakpoints) {
                if (bp.enabled && bp.type == BreakType::Exec && bp.address == pc) {
                    return &bp;
                }
            }
        }
        return nullptr;
    }

    /// Counts a hit on bp and posts it (timeline event + recent hits).
    void RecordHit(Breakpoint& bp, json& stop) {
        ++bp.hits;
        stop["breakpoint"] = bp.id;
        stop["label"] = bp.label;
        stop["type"] = BreakTypeName(bp.type);
        PostStop(&bp, stop);
    }

    /// Records a stop: a timeline event and the recent-hits list. Log-mode breakpoints are
    /// stepped over and the game continues; stop-mode ones (and unknown stops) stay paused.
    void HandleStop(const std::string& packet) {
        state = TargetState::Paused;
        json stop = DescribeStop(packet, "breakpoint");
        Breakpoint* bp = MatchStop(stop);
        if (bp) {
            RecordHit(*bp, stop);
        } else {
            stop["reason"] = "the game stopped (svcBreak or an exception)";
            PostStop(nullptr, stop);
        }
        paused_info = stop;
        if (bp && !bp->stop) {
            if (bp->hits >= bp->max_hits) {
                // A hot breakpoint would slow the game to a crawl: retire it.
                Command(RemovePacket(*bp));
                bp->enabled = false;
                Post(fmt::format("breakpoint #{} disabled after {} hits", bp->id, bp->hits));
            } else {
                StepOverStoppedBreakpoint();
            }
            Continue();
        }
    }

    /// Adds a stop to the recent hits and the timeline (kind "break").
    void PostStop(const Breakpoint* bp, json& stop) {
        stop["frame"] = PresentedFrames();
        recent_hits.push_back(stop);
        if (recent_hits.size() > 50) {
            recent_hits.pop_front();
        }
        std::string text = fmt::format(
            "{} {} at {}", bp ? fmt::format("breakpoint #{}", bp->id) : std::string{"stop"},
            bp ? fmt::format("({} {})", BreakTypeName(bp->type), bp->label) : std::string{},
            stop.contains("pc") ? fmt::format("pc {:#x}", stop["pc"].get<u64>())
                                : std::string{"?"});
        if (stop.contains("data_address")) {
            text += fmt::format(" data {:#x}", stop["data_address"].get<u64>());
        }
        if (stop.contains("backtrace")) {
            for (const auto& f : stop["backtrace"]) {
                text += "\n" + f.get<std::string>();
            }
        }
        if (context.timeline) {
            context.timeline->Push(EventKind::Break, 3, "debugger", std::move(text));
        }
    }

    /// Worker: waits for stop replies while the game runs, and notices a closed connection.
    void Run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::unique_lock lock{mutex};
            if (state == TargetState::Detached) {
                return;
            }
            while (!pending_stops.empty()) {
                const std::string packet = std::move(pending_stops.front());
                pending_stops.pop_front();
                HandleStop(packet);
            }
            if (state == TargetState::Running) {
                if (auto packet = ReadPacket(milliseconds{50})) {
                    if (IsStopReply(*packet)) {
                        HandleStop(*packet);
                    }
                }
            } else if (socket && !socket->IsOpen()) {
                Reset("the GDB stub closed the connection (game stopped?)");
                return;
            }
            if (socket && !socket->IsOpen()) {
                Reset("the GDB stub closed the connection (game stopped?)");
                return;
            }
            lock.unlock();
            std::this_thread::sleep_for(milliseconds{5});
        }
    }

    /// Drops the connection and all client state. Caller holds the mutex.
    void Reset(const std::string& why) {
        if (socket) {
            socket->Close();
        }
        socket.reset();
        state = TargetState::Detached;
        paused_info = nullptr;
        pending_stops.clear();
        breakpoints.clear();
        no_ack = false;
        Post(why);
    }

    void Post(std::string text) {
        if (context.timeline) {
            context.timeline->Push(EventKind::State, 0, "debugger", std::move(text));
        }
    }

    std::mutex mutex;
    TargetState state = TargetState::Detached;
    std::unique_ptr<Socket> socket;
    std::string buffer;
    bool no_ack = false;
    ToolContext context;
    std::vector<Breakpoint> breakpoints;
    u32 next_id = 1;
    json paused_info;
    std::deque<std::string> pending_stops;
    std::deque<json> recent_hits;
    std::jthread worker;
};

// ---- tools ---------------------------------------------------------------------------------

/// debugger_status: whether the stub runs, attachment state, breakpoints and recent hits.
ToolResult DebuggerStatus(ToolContext& ctx, const json&) {
    json out = DebugClient::Get().Status();
    out["stub_setting"] = Settings::values.use_gdbstub.GetValue();
    out["stub_port"] = Settings::values.gdbstub_port.GetValue();
    out["stub_running"] = ctx.system != nullptr && ctx.system->DebuggerEnabled();
    if (!out["stub_running"].get<bool>()) {
        out["hint"] = "Breakpoints need the GDB stub, which starts with the game: settings_set "
                      "use_gdbstub true, then stop and boot the game again.";
    }
    return ToolResult::Structured(std::move(out));
}

/// debugger_attach: connects to the stub (the game keeps running).
ToolResult DebuggerAttach(ToolContext& ctx, const json&) {
    if (std::string error = DebugClient::Get().Attach(ctx); !error.empty()) {
        return ToolResult::Error(error);
    }
    return DebuggerStatus(ctx, json::object());
}

/// debugger_detach: removes all breakpoints, resumes the game and disconnects.
ToolResult DebuggerDetach(ToolContext& ctx, const json&) {
    DebugClient::Get().Detach();
    return DebuggerStatus(ctx, json::object());
}

/// breakpoint_add: attaches if needed, resolves the address and inserts the breakpoint.
ToolResult BreakpointAdd(ToolContext& ctx, const json& args) {
    auto& client = DebugClient::Get();
    if (!client.Attached()) {
        if (std::string error = client.Attach(ctx); !error.empty()) {
            return ToolResult::Error(error);
        }
    }
    Breakpoint bp;
    bp.expr = args.at("address").get<std::string>();
    std::string error;
    const auto address = ResolveGuestAddress(ctx, bp.expr, error);
    if (!address) {
        return ToolResult::Error("address: " + error);
    }
    bp.address = *address;
    const std::string type = args.value("type", "exec");
    bp.type = type == "write"    ? BreakType::Write
              : type == "read"   ? BreakType::Read
              : type == "access" ? BreakType::Access
                                 : BreakType::Exec;
    bp.size = args.value("size", u64{4});
    bp.label = args.value("label", "");
    bp.stop = args.value("mode", "log") == "stop";
    bp.max_hits = args.value("max_hits", u64{1000});
    const u32 id = client.Add(bp, error);
    if (id == 0) {
        return ToolResult::Error(error);
    }
    return ToolResult::Structured(json{
        {"id", id},
        {"address", fmt::format("{:#x}", bp.address)},
        {"note", bp.stop ? "On a hit the game stays paused: inspect (read_memory, debugger_status, "
                           "debugger_step), then debugger_continue."
                         : "Hits are 'break' timeline events (registers + backtrace); the game "
                           "continues. Wait for one with wait_for {kinds:['break']}."}});
}

/// breakpoint_list: the breakpoints with hit counts.
ToolResult BreakpointList(ToolContext& ctx, const json&) {
    json status = DebugClient::Get().Status();
    return ToolResult::Structured(
        json{{"breakpoints", status["breakpoints"]}, {"attached", status["attached"]}});
}

/// breakpoint_remove: removes one breakpoint by id, or all.
ToolResult BreakpointRemove(ToolContext&, const json& args) {
    const u32 id = args.value("all", false) ? 0 : args.value("id", u32{0});
    if (id == 0 && !args.value("all", false)) {
        return ToolResult::Error("give id, or all: true");
    }
    const size_t removed = DebugClient::Get().Remove(id);
    return ToolResult::Structured(json{{"removed", removed}});
}

/// debugger_continue / debugger_pause / debugger_step.
ToolResult DebuggerContinue(ToolContext&, const json&) {
    if (std::string error = DebugClient::Get().Resume(); !error.empty()) {
        return ToolResult::Error(error);
    }
    return ToolResult::Structured(json{{"state", "running"}});
}

ToolResult DebuggerPause(ToolContext&, const json&) {
    if (std::string error = DebugClient::Get().Pause(); !error.empty()) {
        return ToolResult::Error(error);
    }
    return ToolResult::Structured(DebugClient::Get().Status());
}

ToolResult DebuggerStep(ToolContext&, const json&) {
    std::string error;
    json stop = DebugClient::Get().Step(error);
    if (stop.is_null()) {
        return ToolResult::Error(error);
    }
    return ToolResult::Structured(std::move(stop));
}

json EmptySchema() {
    return json{{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}};
}

} // namespace

json DebuggerStatusJson() {
    return DebugClient::Get().Status();
}

void StopDebuggerWork() {
    DebugClient::Get().Detach();
}

std::string AttachDebuggerAfterBoot(ToolContext& ctx) {
    std::string error = "the GDB stub did not start";
    const auto deadline = steady_clock::now() + seconds{30};
    while (steady_clock::now() < deadline && !ServerStopping()) {
        if (ctx.system != nullptr && ctx.system->DebuggerEnabled()) {
            error = DebugClient::Get().Attach(ctx);
            if (error.empty()) {
                return {};
            }
        }
        std::this_thread::sleep_for(milliseconds{250});
    }
    return error;
}

void RegisterDebuggerTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "debugger_status",
        .title = "Debugger status",
        .description =
            "Breakpoint debugger state: whether the emulator's GDB stub runs (it must be on when "
            "the game boots: settings_set use_gdbstub true, then reboot the game), whether the "
            "MCP is attached, running or paused (and where), breakpoints with hit counts, and "
            "recent hits with registers and backtraces.",
        .input_schema = EmptySchema(),
        .annotations = {.read_only = true},
        .handler = DebuggerStatus,
    });
    registry.Add(Tool{
        .name = "debugger_attach",
        .title = "Attach the debugger",
        .description = "Connects the MCP to the emulator's GDB stub (breakpoint_add does this by "
                       "itself). The game keeps running. Only one debugger can be attached: an "
                       "external GDB blocks this and the other way round.",
        .input_schema = EmptySchema(),
        .requires_game = true,
        .requires_control = true,
        .handler = DebuggerAttach,
    });
    registry.Add(Tool{
        .name = "debugger_detach",
        .title = "Detach the debugger",
        .description = "Removes all breakpoints, lets the game run and disconnects from the stub.",
        .input_schema = EmptySchema(),
        .requires_control = true,
        .handler = DebuggerDetach,
    });
    registry.Add(Tool{
        .name = "breakpoint_add",
        .title = "Add a breakpoint or watchpoint",
        .description =
            "Stops when code runs at an address (type exec) or when memory is written, read or "
            "accessed (type write/read/access, size bytes). address: hex or Module+0xoffset "
            "(pointer chains like probes allow). mode log (default): record a 'break' timeline "
            "event with registers and a symbolized backtrace, then continue; after max_hits the "
            "breakpoint retires. mode stop: leave the game paused for inspection, then "
            "debugger_continue. Typical use: find a value with scan_*, then a write watchpoint "
            "on it shows which code changes it.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"address", {{"type", "string"}}},
                           {"type",
                            {{"type", "string"}, {"enum", {"exec", "write", "read", "access"}}}},
                           {"size", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8}}},
                           {"label", {{"type", "string"}}},
                           {"mode", {{"type", "string"}, {"enum", {"log", "stop"}}}},
                           {"max_hits", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100000}}}}},
                         {"required", {"address"}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = BreakpointAdd,
    });
    registry.Add(Tool{
        .name = "breakpoint_list",
        .title = "List breakpoints",
        .description = "Breakpoints and watchpoints with their hit counts.",
        .input_schema = EmptySchema(),
        .annotations = {.read_only = true},
        .handler = BreakpointList,
    });
    registry.Add(Tool{
        .name = "breakpoint_remove",
        .title = "Remove breakpoints",
        .description = "Removes one breakpoint by id, or all of them with all: true.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"id", {{"type", "integer"}, {"minimum", 1}}},
                           {"all", {{"type", "boolean"}}}}},
                         {"additionalProperties", false}},
        .requires_control = true,
        .handler = BreakpointRemove,
    });
    registry.Add(Tool{
        .name = "debugger_continue",
        .title = "Continue after a stop",
        .description = "Resumes a game the debugger paused (a stop-mode hit, debugger_pause or "
                       "debugger_step), stepping over the breakpoint it stopped on.",
        .input_schema = EmptySchema(),
        .requires_game = true,
        .requires_control = true,
        .handler = DebuggerContinue,
    });
    registry.Add(Tool{
        .name = "debugger_pause",
        .title = "Pause through the debugger",
        .description = "Stops all guest threads through the debugger (like Ctrl-C in GDB), so "
                       "debugger_step can single-step. Resume with debugger_continue.",
        .input_schema = EmptySchema(),
        .requires_game = true,
        .requires_control = true,
        .handler = DebuggerPause,
    });
    registry.Add(Tool{
        .name = "debugger_step",
        .title = "Single-step",
        .description = "Executes one instruction of the thread the debugger stopped on and stays "
                       "paused; returns its registers and backtrace.",
        .input_schema = EmptySchema(),
        .requires_game = true,
        .requires_control = true,
        .handler = DebuggerStep,
    });
}

} // namespace Mcp
