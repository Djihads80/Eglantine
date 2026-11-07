// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "common/common_types.h"

/// Transport of the debug server (docs/nx2/debug-mcp.md): newline-delimited JSON requests over a
/// loopback TCP socket, served on one thread. It knows nothing about the emulator; the tools are
/// the handlers it is given (nx2_debug_tools.cpp).
namespace Core::Nx2Debug {

struct Endpoint {
    std::string host;
    u16 port;
};

/// Parses EDEN_NX2_DEBUG_SERVER: "<port>" or "<host>:<port>". Only loopback hosts are accepted.
[[nodiscard]] std::optional<Endpoint> ParseEndpoint(std::string_view text, std::string& error);

/// Whether the client of the request being handled has disconnected. A handler that waits polls
/// it to undo what it started (input injection releases its buttons).
using ClientGone = std::function<bool()>;

/// A tool: takes the request's params, returns its result, throws std::exception on failure.
using Handler =
    std::function<nlohmann::json(const nlohmann::json& params, const ClientGone& client_gone)>;
using Handlers = std::map<std::string, Handler, std::less<>>;

/// Answers one request line, {"id":..,"method":..,"params":{..}}, with one response line (no
/// newline): {"id":..,"result":..} or {"id":..,"error":"..."}.
[[nodiscard]] std::string HandleRequest(
    const Handlers& handlers, std::string_view line,
    const ClientGone& client_gone = [] { return false; });

[[nodiscard]] std::string Base64(std::span<const u8> data);

class Server {
public:
    /// Listens on the endpoint (port 0 picks a free one); throws std::exception when it cannot.
    explicit Server(const Endpoint& endpoint, Handlers handlers);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    [[nodiscard]] u16 Port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Core::Nx2Debug
