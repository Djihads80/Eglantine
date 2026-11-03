// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/socket_types.h"
#include "core/internal_network/socket_proxy.h"

namespace {

void BindSocket(Network::ProxySocket& socket, Network::Protocol protocol) {
    const auto type =
        protocol == Network::Protocol::TCP ? Network::Type::STREAM : Network::Type::DGRAM;
    REQUIRE(socket.Initialize(Network::Domain::INET, type, protocol) == Network::Errno::SUCCESS);
    REQUIRE(socket.Bind({.family = Network::Domain::INET,
                         .ip = {192, 168, 1, 1},
                         .portno = 35000}) == Network::Errno::SUCCESS);
}

template <std::size_t N>
void QueuePacket(Network::ProxySocket& socket, Network::Protocol protocol,
                 const std::array<u8, N>& payload) {
    Network::ProxyPacket packet{};
    packet.local_endpoint = {.family = Network::Domain::INET,
                             .ip = {192, 168, 1, 2},
                             .portno = 35000};
    packet.remote_endpoint = {.family = Network::Domain::INET,
                              .ip = {192, 168, 1, 1},
                              .portno = 35000};
    packet.protocol = protocol;
    packet.data.assign(payload.begin(), payload.end());
    socket.HandleProxyPacket(packet);
}

} // namespace

TEST_CASE("Room UDP sockets receive queued datagrams with recv", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::UDP);
    REQUIRE(socket.SetNonBlock(true) == Network::Errno::SUCCESS);

    const std::array<u8, 4> payload{1, 2, 3, 4};
    QueuePacket(socket, Network::Protocol::UDP, payload);

    std::array<u8, 4> received{};
    const auto [count, error] = socket.Recv(0, received);
    REQUIRE(error == Network::Errno::SUCCESS);
    REQUIRE(count == payload.size());
    REQUIRE(received == payload);

    const auto [empty_count, empty_error] = socket.Recv(0, received);
    REQUIRE(empty_count == -1);
    REQUIRE(empty_error == Network::Errno::AGAIN);
}

TEST_CASE("Room sockets peek without consuming", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::UDP);
    REQUIRE(socket.SetNonBlock(true) == Network::Errno::SUCCESS);

    const std::array<u8, 4> payload{1, 2, 3, 4};
    QueuePacket(socket, Network::Protocol::UDP, payload);

    std::array<u8, 4> received{};
    REQUIRE(socket.Recv(Network::FLAG_MSG_PEEK, received) ==
            std::pair<s32, Network::Errno>{4, Network::Errno::SUCCESS});
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{4, Network::Errno::SUCCESS});
    REQUIRE(received == payload);
    REQUIRE(socket.Recv(0, received).second == Network::Errno::AGAIN);
}

TEST_CASE("Room UDP sockets truncate oversized datagrams", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::UDP);
    REQUIRE(socket.SetNonBlock(true) == Network::Errno::SUCCESS);

    QueuePacket(socket, Network::Protocol::UDP, std::array<u8, 4>{1, 2, 3, 4});

    std::array<u8, 2> received{};
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{2, Network::Errno::SUCCESS});
    REQUIRE(received == std::array<u8, 2>{1, 2});
    REQUIRE(socket.Recv(0, received).second == Network::Errno::AGAIN);
}

TEST_CASE("Room TCP sockets keep the unread part of a packet", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::TCP);
    REQUIRE(socket.SetNonBlock(true) == Network::Errno::SUCCESS);

    QueuePacket(socket, Network::Protocol::TCP, std::array<u8, 6>{1, 2, 3, 4, 5, 6});

    std::array<u8, 4> received{};
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{4, Network::Errno::SUCCESS});
    REQUIRE(received == std::array<u8, 4>{1, 2, 3, 4});
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{2, Network::Errno::SUCCESS});
    REQUIRE(received[0] == 5);
    REQUIRE(received[1] == 6);
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{0, Network::Errno::SUCCESS});
}

TEST_CASE("Room TCP sockets read a packet in small parts and peek at the unread part",
          "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::TCP);
    REQUIRE(socket.SetNonBlock(true) == Network::Errno::SUCCESS);

    QueuePacket(socket, Network::Protocol::TCP, std::array<u8, 5>{1, 2, 3, 4, 5});
    QueuePacket(socket, Network::Protocol::TCP, std::array<u8, 2>{6, 7});

    std::array<u8, 2> received{};
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{2, Network::Errno::SUCCESS});
    REQUIRE(received == std::array<u8, 2>{1, 2});
    REQUIRE(socket.Recv(Network::FLAG_MSG_PEEK, received) ==
            std::pair<s32, Network::Errno>{2, Network::Errno::SUCCESS});
    REQUIRE(received == std::array<u8, 2>{3, 4});
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{2, Network::Errno::SUCCESS});
    REQUIRE(received == std::array<u8, 2>{3, 4});
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{1, Network::Errno::SUCCESS});
    REQUIRE(received[0] == 5);
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{2, Network::Errno::SUCCESS});
    REQUIRE(received == std::array<u8, 2>{6, 7});
}

TEST_CASE("Room TCP sockets report an empty queue as end of stream", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::TCP);

    std::array<u8, 4> received{};
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{0, Network::Errno::SUCCESS});
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
}

TEST_CASE("Blocking room sockets wait for packets and time out with EAGAIN", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::UDP);
    REQUIRE(socket.SetRcvTimeo(50) == Network::Errno::SUCCESS);

    std::array<u8, 4> received{};
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{-1, Network::Errno::AGAIN});
    REQUIRE(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds{50});

    REQUIRE(socket.SetRcvTimeo(5000) == Network::Errno::SUCCESS);
    const std::array<u8, 4> payload{1, 2, 3, 4};
    std::jthread sender([&socket, &payload] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        QueuePacket(socket, Network::Protocol::UDP, payload);
    });
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{4, Network::Errno::SUCCESS});
    REQUIRE(received == payload);
}

TEST_CASE("Closing a room socket wakes a blocked recv and drops queued packets", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::UDP);
    REQUIRE(socket.SetRcvTimeo(5000) == Network::Errno::SUCCESS);

    std::array<u8, 4> received{};
    const auto start = std::chrono::steady_clock::now();
    std::jthread closer([&socket] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        socket.Close();
    });
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{-1, Network::Errno::BADF});
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
    closer.join();

    Network::ProxySocket queued;
    BindSocket(queued, Network::Protocol::UDP);
    QueuePacket(queued, Network::Protocol::UDP, std::array<u8, 4>{1, 2, 3, 4});
    REQUIRE(queued.Close() == Network::Errno::SUCCESS);
    QueuePacket(queued, Network::Protocol::UDP, std::array<u8, 4>{5, 6, 7, 8});
    REQUIRE(queued.Recv(0, received) == std::pair<s32, Network::Errno>{-1, Network::Errno::BADF});
}

TEST_CASE("Room stream sockets with the default protocol keep stream behavior", "[core][sockets]") {
    // socket(AF_INET, SOCK_STREAM, 0) leaves the protocol unspecified
    Network::ProxySocket socket;
    REQUIRE(socket.Initialize(Network::Domain::INET, Network::Type::STREAM,
                              Network::Protocol::Unspecified) == Network::Errno::SUCCESS);
    REQUIRE(socket.Bind({.family = Network::Domain::INET,
                         .ip = {192, 168, 1, 1},
                         .portno = 35000}) == Network::Errno::SUCCESS);
    REQUIRE(socket.SetRcvTimeo(1000) == Network::Errno::SUCCESS);

    std::array<u8, 2> received{};
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{0, Network::Errno::SUCCESS});
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::milliseconds{500});

    QueuePacket(socket, Network::Protocol::Unspecified, std::array<u8, 4>{1, 2, 3, 4});
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{2, Network::Errno::SUCCESS});
    REQUIRE(received == std::array<u8, 2>{1, 2});
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{2, Network::Errno::SUCCESS});
    REQUIRE(received == std::array<u8, 2>{3, 4});
}

TEST_CASE("One room packet wakes every blocked peek reader", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::UDP);
    REQUIRE(socket.SetRcvTimeo(2000) == Network::Errno::SUCCESS);

    // Catch2 assertions are not thread-safe here, so the readers only record their results
    std::array<std::pair<s32, Network::Errno>, 2> results{};
    std::array<std::chrono::steady_clock::time_point, 2> finished{};
    std::chrono::steady_clock::time_point queued{};
    {
        std::array<std::jthread, 2> readers;
        for (std::size_t i = 0; i < readers.size(); ++i) {
            readers[i] = std::jthread([&socket, &results, &finished, i] {
                std::array<u8, 4> peeked{};
                results[i] = socket.Recv(Network::FLAG_MSG_PEEK, peeked);
                finished[i] = std::chrono::steady_clock::now();
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        queued = std::chrono::steady_clock::now();
        QueuePacket(socket, Network::Protocol::UDP, std::array<u8, 4>{1, 2, 3, 4});
    }

    for (std::size_t i = 0; i < results.size(); ++i) {
        REQUIRE(results[i] == std::pair<s32, Network::Errno>{4, Network::Errno::SUCCESS});
        REQUIRE(finished[i] - queued < std::chrono::milliseconds{1000});
    }
}
