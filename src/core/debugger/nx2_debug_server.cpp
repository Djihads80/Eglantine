// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <charconv>
#include <thread>

#include <boost/asio.hpp>

#include "common/logging.h"
#include "common/thread.h"
#include "core/debugger/nx2_debug_server.h"

namespace Core::Nx2Debug {
namespace {

using boost::asio::ip::tcp;

/// Longest request line; a client sending more is disconnected.
constexpr size_t MAX_REQUEST = size_t{1} << 20;

class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(tcp::socket socket_, const Handlers& handlers_)
        : socket{std::move(socket_)}, handlers{handlers_} {}

    void Read() {
        boost::asio::async_read_until(
            socket, boost::asio::dynamic_buffer(input, MAX_REQUEST), '\n',
            [self = shared_from_this()](const boost::system::error_code& error, size_t length) {
                if (error) {
                    return;
                }
                std::string_view line{self->input.data(), length - 1};
                if (!line.empty() && line.back() == '\r') {
                    line.remove_suffix(1);
                }
                self->output = line.empty() ? std::string{}
                                            : HandleRequest(self->handlers, line,
                                                            [&self] { return self->ClientGone(); });
                self->input.erase(0, length);
                if (self->output.empty()) {
                    self->Read();
                    return;
                }
                self->output += '\n';
                self->Write();
            });
    }

private:
    /// A non-blocking peek: end of stream or a reset means the client left.
    bool ClientGone() {
        boost::system::error_code error;
        socket.non_blocking(true, error);
        std::array<char, 1> byte;
        const size_t length =
            socket.receive(boost::asio::buffer(byte), tcp::socket::message_peek, error);
        return error ? error != boost::asio::error::would_block : length == 0;
    }

    void Write() {
        boost::asio::async_write(
            socket, boost::asio::buffer(output),
            [self = shared_from_this()](const boost::system::error_code& error, size_t) {
                if (!error) {
                    self->Read();
                }
            });
    }

    tcp::socket socket;
    const Handlers& handlers;
    std::string input;
    std::string output;
};

} // Anonymous namespace

std::optional<Endpoint> ParseEndpoint(std::string_view text, std::string& error) {
    std::string_view host = "127.0.0.1";
    std::string_view port_text = text;
    if (const size_t colon = text.rfind(':'); colon != std::string_view::npos) {
        host = text.substr(0, colon);
        port_text = text.substr(colon + 1);
        if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
            host = host.substr(1, host.size() - 2);
        }
    }
    if (host == "localhost") {
        host = "127.0.0.1";
    }
    if (host != "127.0.0.1" && host != "::1") {
        error = fmt::format(
            "host {} is not loopback; only 127.0.0.1, ::1 and localhost are allowed", host);
        return std::nullopt;
    }
    unsigned port = 0;
    const auto [end, result] =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (port_text.empty() || result != std::errc{} || end != port_text.data() + port_text.size() ||
        port > 65535) {
        error = fmt::format("bad port \"{}\"", port_text);
        return std::nullopt;
    }
    return Endpoint{std::string{host}, static_cast<u16>(port)};
}

std::string HandleRequest(const Handlers& handlers, std::string_view line,
                          const ClientGone& client_gone) {
    nlohmann::json response = {{"id", nullptr}};
    try {
        const nlohmann::json request = nlohmann::json::parse(line);
        if (!request.is_object()) {
            throw std::runtime_error{"request is not an object"};
        }
        response["id"] = request.value("id", nlohmann::json{});
        const std::string method = request.value("method", std::string{});
        const auto it = handlers.find(method);
        if (it == handlers.end()) {
            throw std::runtime_error{fmt::format("unknown method \"{}\"", method)};
        }
        const nlohmann::json params = request.value("params", nlohmann::json::object());
        if (!params.is_object()) {
            throw std::runtime_error{"params is not an object"};
        }
        response["result"] = it->second(params, client_gone);
    } catch (const std::exception& error) {
        response = {{"id", response["id"]}, {"error", error.what()}};
    }
    return response.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string Base64(std::span<const u8> data) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t index = 0; index < data.size(); index += 3) {
        const size_t left = data.size() - index;
        const u32 chunk = (u32{data[index]} << 16) | (left > 1 ? u32{data[index + 1]} << 8 : 0) |
                          (left > 2 ? u32{data[index + 2]} : 0);
        out += alphabet[(chunk >> 18) & 63];
        out += alphabet[(chunk >> 12) & 63];
        out += left > 1 ? alphabet[(chunk >> 6) & 63] : '=';
        out += left > 2 ? alphabet[chunk & 63] : '=';
    }
    return out;
}

struct Server::Impl {
    explicit Impl(const Endpoint& endpoint, Handlers handlers_)
        : handlers{std::move(handlers_)}, acceptor{io} {
        const tcp::endpoint address{boost::asio::ip::make_address(endpoint.host), endpoint.port};
        acceptor.open(address.protocol());
#ifndef _WIN32
        // Rebinding after a relaunch while the old socket is in TIME_WAIT. On Windows the option
        // would let a second instance bind the same port instead.
        acceptor.set_option(tcp::acceptor::reuse_address(true));
#endif
        acceptor.bind(address);
        acceptor.listen();
        Accept();
        thread = std::thread{[this] {
            Common::SetCurrentThreadName("DebugServer");
            io.run();
        }};
    }

    ~Impl() {
        io.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    void Accept() {
        acceptor.async_accept([this](const boost::system::error_code& error, tcp::socket socket) {
            if (error) {
                return;
            }
            std::make_shared<Session>(std::move(socket), handlers)->Read();
            Accept();
        });
    }

    Handlers handlers;
    boost::asio::io_context io;
    tcp::acceptor acceptor;
    std::thread thread;
};

Server::Server(const Endpoint& endpoint, Handlers handlers)
    : impl{std::make_unique<Impl>(endpoint, std::move(handlers))} {}

Server::~Server() = default;

u16 Server::Port() const noexcept {
    boost::system::error_code error;
    return impl->acceptor.local_endpoint(error).port();
}

} // namespace Core::Nx2Debug
