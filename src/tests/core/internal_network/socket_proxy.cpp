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
    REQUIRE(
        socket.Bind({.family = Network::Domain::INET, .ip = {192, 168, 1, 1}, .portno = 35000}) ==
        Network::Errno::SUCCESS);
}

Network::ProxyPacket MakePacket(Network::Protocol protocol) {
    Network::ProxyPacket packet{};
    packet.local_endpoint = {
        .family = Network::Domain::INET, .ip = {192, 168, 1, 2}, .portno = 35000};
    packet.remote_endpoint = {
        .family = Network::Domain::INET, .ip = {192, 168, 1, 1}, .portno = 35000};
    packet.protocol = protocol;
    return packet;
}

template <std::size_t N>
void QueuePacket(Network::ProxySocket& socket, Network::Protocol protocol,
                 const std::array<u8, N>& payload) {
    Network::ProxyPacket packet = MakePacket(protocol);
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
    REQUIRE(count == static_cast<int>(payload.size()));
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

TEST_CASE("Closing a room socket wakes a blocked recv and drops queued packets",
          "[core][sockets]") {
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
    REQUIRE(
        socket.Bind({.family = Network::Domain::INET, .ip = {192, 168, 1, 1}, .portno = 35000}) ==
        Network::Errno::SUCCESS);
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

TEST_CASE("Room sockets accept only packets for their port and protocol", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::UDP);

    REQUIRE(socket.AcceptsProxyPacket(MakePacket(Network::Protocol::UDP)));
    REQUIRE_FALSE(socket.AcceptsProxyPacket(MakePacket(Network::Protocol::TCP)));

    Network::ProxyPacket other_port = MakePacket(Network::Protocol::UDP);
    other_port.remote_endpoint.portno = 35001;
    REQUIRE_FALSE(socket.AcceptsProxyPacket(other_port));

    Network::ProxyPacket broadcast = MakePacket(Network::Protocol::UDP);
    broadcast.broadcast = true;
    REQUIRE(socket.AcceptsProxyPacket(broadcast));

    Network::ProxyPacket other_address = MakePacket(Network::Protocol::UDP);
    other_address.remote_endpoint.ip = {192, 168, 1, 3};
    REQUIRE_FALSE(socket.AcceptsProxyPacket(other_address));

    REQUIRE(socket.Close() == Network::Errno::SUCCESS);
    REQUIRE_FALSE(socket.AcceptsProxyPacket(MakePacket(Network::Protocol::UDP)));
}

TEST_CASE("Room UDP sockets allocate ephemeral ports and report their bound endpoint",
          "[core][sockets]") {
    Network::ProxySocket socket;
    REQUIRE(socket.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::Unspecified) == Network::Errno::SUCCESS);
    REQUIRE(socket.Bind({.family = Network::Domain::INET, .ip = {}, .portno = 0}) ==
            Network::Errno::SUCCESS);

    const auto [endpoint, error] = socket.GetSockName();
    REQUIRE(error == Network::Errno::SUCCESS);
    REQUIRE(endpoint.family == Network::Domain::INET);
    REQUIRE(endpoint.ip == Network::IPv4Address{});
    REQUIRE(endpoint.portno >= 49152);

    Network::ProxyPacket packet{};
    packet.local_endpoint = {
        .family = Network::Domain::INET, .ip = {192, 168, 1, 2}, .portno = 12345};
    packet.remote_endpoint = endpoint;
    packet.protocol = Network::Protocol::UDP;
    REQUIRE(socket.AcceptsProxyPacket(packet));
    REQUIRE(socket.Bind(endpoint) == Network::Errno::INVAL);
}

TEST_CASE("Emulated ICMP sockets return echo replies without raw socket privileges",
          "[core][sockets]") {
    Network::ProxySocket socket;
    REQUIRE(socket.Initialize(Network::Domain::INET, Network::Type::RAW, Network::Protocol::ICMP) ==
            Network::Errno::SUCCESS);
    REQUIRE(socket.SetNonBlock(true) == Network::Errno::SUCCESS);

    const std::array<u8, 8> request{8, 0, 0xf7, 0xff, 0, 1, 0, 1};
    const Network::SockAddrIn destination{
        .family = Network::Domain::INET,
        .ip = {1, 1, 1, 1},
        .portno = 0,
    };
    REQUIRE(socket.SendTo(0, request, &destination) ==
            std::pair<s32, Network::Errno>{8, Network::Errno::SUCCESS});

    const auto ready = socket.EmulatedPoll(Network::PollEvents::In);
    REQUIRE(ready.has_value());
    REQUIRE(True(*ready & Network::PollEvents::In));

    std::array<u8, 8> reply{};
    Network::SockAddrIn source{};
    REQUIRE(socket.RecvFrom(0, reply, &source) ==
            std::pair<s32, Network::Errno>{8, Network::Errno::SUCCESS});
    REQUIRE(reply[0] == 0);
    REQUIRE(reply[1] == 0);
    REQUIRE(source.ip == destination.ip);
}

TEST_CASE("Connected room UDP sockets send through their peer endpoint", "[core][sockets]") {
    Network::ProxySocket socket;
    REQUIRE(socket.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::UDP) == Network::Errno::SUCCESS);

    const Network::SockAddrIn peer{
        .family = Network::Domain::INET,
        .ip = {192, 168, 1, 2},
        .portno = 12345,
    };
    REQUIRE(socket.Connect(peer) == Network::Errno::SUCCESS);
    const auto [connected_peer, error] = socket.GetPeerName();
    REQUIRE(error == Network::Errno::SUCCESS);
    REQUIRE(connected_peer.family == peer.family);
    REQUIRE(connected_peer.ip == peer.ip);
    REQUIRE(connected_peer.portno == peer.portno);
}

TEST_CASE("Native sockets accept no room packets", "[core][sockets]") {
    Network::Socket socket;
    REQUIRE_FALSE(socket.AcceptsProxyPacket(MakePacket(Network::Protocol::UDP)));
}
