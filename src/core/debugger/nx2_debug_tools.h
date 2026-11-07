// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/common_types.h"
#include "hid_core/frontend/emulated_controller.h"

namespace Core {
class System;
}

namespace Core::Memory {
class Memory;
}

namespace Core::Nx2Debug {

class Server;
struct Endpoint;
struct ToolState;

/// The debug server of one boot (docs/nx2/debug-mcp.md): started when a game has loaded, stopped
/// first when it shuts down, so the tools never see a half-built or half-destroyed system.
class Service {
public:
    explicit Service(System& system, std::string title, u64 program_id, const Endpoint& endpoint,
                     std::filesystem::path dump_directory);
    ~Service();

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

private:
    std::shared_ptr<ToolState> state;
    std::unique_ptr<Server> server;
};

/// A reject_shaders hash: exactly 16 hex digits, optionally after 0x. Throws std::runtime_error.
[[nodiscard]] u64 ParseShaderHash(std::string_view text);

/// search_memory's pattern: hex digit pairs, optionally after 0x, spaces allowed. Throws
/// std::runtime_error.
[[nodiscard]] std::vector<u8> ParseHexBytes(std::string_view text);

/// read_memory's formats: {"hex": "..."} or {"u32"|"u64"|"f32": [...]}; u64 values are hex strings
/// and non-finite floats are "nan", "inf" or "-inf". Throws std::runtime_error when the format is
/// unknown or the size is not a multiple of its element.
[[nodiscard]] nlohmann::json FormatMemory(std::span<const u8> bytes, std::string_view as);

/// Copies guest memory from a thread that is not a guest CPU core (docs/nx2/debug-mcp.md, "Guest
/// and GPU memory"). Returns the first address that is not mapped; out is then partly written.
[[nodiscard]] std::optional<u64> ReadGuestMemory(Memory::Memory& memory, u64 address,
                                                 std::span<u8> out);

struct GuestSearch {
    std::vector<u64> hits;
    /// Where to resume: the first address no match was looked for at; end when complete.
    u64 next;
    bool complete;
    /// Mapped bytes read.
    u64 scanned;
};

/// Finds the pattern in the mapped pages of [start, end), as ReadGuestMemory reads them. Stops at
/// max_hits or the deadline. Matches may span pages, but not unmapped gaps.
[[nodiscard]] GuestSearch SearchGuestMemory(Memory::Memory& memory, std::span<const u8> pattern,
                                            u64 start, u64 end, size_t max_hits,
                                            std::chrono::steady_clock::time_point deadline);

/// One step of input or input_sequence: what to hold, for how many presented frames.
struct InputStep {
    HID::InjectedInput input;
    u64 frames;
};

/// {buttons?, left_stick?, right_stick?, frames}. Sticks are [x, y] in -1..1. Throws
/// std::runtime_error on unknown keys, buttons or out-of-range values.
[[nodiscard]] InputStep ParseInputStep(const nlohmann::json& step);

/// The input of the step active at present count now, for steps starting at present count start;
/// nothing before start and from the end of the last step on.
[[nodiscard]] std::optional<HID::InjectedInput> InputAt(std::span<const InputStep> steps, u64 start,
                                                        u64 now);

/// The service when EDEN_NX2_DEBUG_SERVER is set and it can listen, otherwise null.
[[nodiscard]] std::unique_ptr<Service> MaybeStartService(System& system, std::string title,
                                                         u64 program_id);

} // namespace Core::Nx2Debug
