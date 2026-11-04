// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <future>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"

TEST_CASE("Network::Errors", "[core]") {
    Network::NetworkInstance network_instance; // initialize network

    Network::Socket socks[2];
    for (Network::Socket& sock : socks) {
        REQUIRE(sock.Initialize(Network::Domain::INET, Network::Type::STREAM,
                                Network::Protocol::TCP) == Network::Errno::SUCCESS);
    }

    Network::SockAddrIn addr{
        Network::Domain::INET,
        {127, 0, 0, 1},
        1, // hopefully nobody running this test has something listening on port 1
    };
    REQUIRE(socks[0].Connect(addr) == Network::Errno::CONNREFUSED);

    std::vector<u8> message{1, 2, 3, 4};
    REQUIRE(socks[1].Recv(0, message).second == Network::Errno::NOTCONN);
}

TEST_CASE("Network::PerCallNonblockingReceive", "[core][sockets]") {
    using namespace std::chrono_literals;
    Network::NetworkInstance network_instance;
    Network::Socket receiver;
    Network::Socket sender;
    REQUIRE(receiver.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                                Network::Protocol::UDP) == Network::Errno::SUCCESS);
    REQUIRE(sender.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::UDP) == Network::Errno::SUCCESS);
    REQUIRE(receiver.Bind({Network::Domain::INET, {127, 0, 0, 1}, 0}) ==
            Network::Errno::SUCCESS);
    REQUIRE(receiver.SetRcvTimeo(2000) == Network::Errno::SUCCESS);
    const auto [address, address_error] = receiver.GetSockName();
    REQUIRE(address_error == Network::Errno::SUCCESS);

    auto blocking = std::async(std::launch::async, [&] {
        std::array<u8, 1> buffer{};
        const auto [size, error] = receiver.RecvFrom(0, buffer, nullptr);
        return std::pair{std::pair{size, error}, buffer[0]};
    });
    const bool remained_blocked = blocking.wait_for(100ms) == std::future_status::timeout;
    auto no_wait = std::async(std::launch::async, [&] {
        std::array<u8, 1> buffer{};
        return receiver.RecvFrom(Network::FLAG_MSG_DONTWAIT, buffer, nullptr);
    });
    const bool returned_promptly = no_wait.wait_for(1s) == std::future_status::ready;
    const std::array<u8, 1> data{'x'};
    const auto first_send = sender.SendTo(0, data, &address);
    const auto second_send = sender.SendTo(0, data, &address);
    const auto no_wait_result = no_wait.get();
    const auto [blocking_result, received] = blocking.get();

    CHECK(remained_blocked);
    CHECK(returned_promptly);
    CHECK(no_wait_result.first == -1);
    CHECK(no_wait_result.second == Network::Errno::AGAIN);
    CHECK(first_send.second == Network::Errno::SUCCESS);
    CHECK(second_send.second == Network::Errno::SUCCESS);
    CHECK(blocking_result.first == 1);
    CHECK(blocking_result.second == Network::Errno::SUCCESS);
    CHECK(received == 'x');
}

namespace {

struct PrivateServerValues {
    bool enabled;
    std::string domains;
    std::string gateway;
    std::string relay;
};

/// Sets the private-server settings, which are restored with this.
class ScopedPrivateServer {
public:
    explicit ScopedPrivateServer(const PrivateServerValues& values)
        : before{Settings::values.private_server_enabled.GetValue(),
                 Settings::values.private_server_domains.GetValue(),
                 Settings::values.private_server_gateway.GetValue(),
                 Settings::values.private_server_relay.GetValue()} {
        Set(values);
    }

    ~ScopedPrivateServer() {
        Set(before);
    }

    ScopedPrivateServer(const ScopedPrivateServer&) = delete;
    ScopedPrivateServer& operator=(const ScopedPrivateServer&) = delete;

private:
    static void Set(const PrivateServerValues& values) {
        Settings::values.private_server_enabled.SetValue(values.enabled);
        Settings::values.private_server_domains.SetValue(values.domains);
        Settings::values.private_server_gateway.SetValue(values.gateway);
        Settings::values.private_server_relay.SetValue(values.relay);
    }

    PrivateServerValues before;
};

constexpr const char* Domains = "example.test,Game.Example.ORG";
constexpr const char* Gateway = "127.0.0.1";
constexpr const char* Relay = "203.0.113.1:3478,49170-49233";

Network::PrivateServerRoute HostRoute(std::string_view host) {
    return Network::RoutePrivateServerHost(host).route;
}

Network::PrivateServerRoute DestinationRoute(Network::IPv4Address ip, u16 port) {
    return Network::RoutePrivateServerDestination({Network::Domain::INET, ip, port}).route;
}

} // namespace

TEST_CASE("Network::PrivateServerDisabled", "[core][network]") {
    using Network::PrivateServerRoute;
    {
        const ScopedPrivateServer settings{{false, Domains, Gateway, Relay}};
        CHECK(HostRoute("example.test") == PrivateServerRoute::Disabled);
        CHECK(DestinationRoute({8, 8, 8, 8}, 53) == PrivateServerRoute::Disabled);
    }
    // Malformed settings are ignored while the mode is off.
    const ScopedPrivateServer settings{{false, "bad..name", "10.0.0.1", "relay"}};
    CHECK(HostRoute("example.test") == PrivateServerRoute::Disabled);
    CHECK(DestinationRoute({8, 8, 8, 8}, 53) == PrivateServerRoute::Disabled);
}

TEST_CASE("Network::PrivateServerHostRouting", "[core][network]") {
    using Network::PrivateServerRoute;
    const ScopedPrivateServer settings{{true, Domains, Gateway, Relay}};

    for (const char* host : {"example.test", "api.example.test", "API.Example.TEST", "example.test.",
                             "game.example.org", "a.b.GAME.example.org"}) {
        INFO(host);
        const Network::PrivateServerRouting routing = Network::RoutePrivateServerHost(host);
        CHECK(routing.route == PrivateServerRoute::Routed);
        CHECK(routing.gateway == Network::IPv4Address{127, 0, 0, 1});
    }
    for (const char* host : {"notexample.test", "example.test.evil.net", "example.org", "test",
                             "example.test..", "srv.nintendo.net", ""}) {
        INFO(host);
        CHECK(HostRoute(host) == PrivateServerRoute::Unrouted);
    }
}

TEST_CASE("Network::PrivateServerDestinationRouting", "[core][network]") {
    using Network::PrivateServerRoute;
    {
        const ScopedPrivateServer settings{{true, Domains, Gateway, Relay}};
        for (const int port : {1, 443, 65535}) {
            CHECK(DestinationRoute({127, 0, 0, 1}, static_cast<u16>(port)) ==
                  PrivateServerRoute::Routed);
        }
        for (const int port : {3478, 49170, 49200, 49233}) {
            CHECK(DestinationRoute({203, 0, 113, 1}, static_cast<u16>(port)) ==
                  PrivateServerRoute::Routed);
        }
        for (const int port : {443, 3477, 3479, 49169, 49234}) {
            CHECK(DestinationRoute({203, 0, 113, 1}, static_cast<u16>(port)) ==
                  PrivateServerRoute::Unrouted);
        }
        CHECK(DestinationRoute({203, 0, 113, 2}, 3478) == PrivateServerRoute::Unrouted);
        CHECK(DestinationRoute({127, 0, 0, 2}, 443) == PrivateServerRoute::Unrouted);
        CHECK(DestinationRoute({8, 8, 8, 8}, 53) == PrivateServerRoute::Unrouted);
    }
    {
        // Without a relay, only the gateway is reachable.
        const ScopedPrivateServer settings{{true, Domains, Gateway, ""}};
        CHECK(DestinationRoute({127, 0, 0, 1}, 443) == PrivateServerRoute::Routed);
        CHECK(DestinationRoute({203, 0, 113, 1}, 3478) == PrivateServerRoute::Unrouted);
    }
    const ScopedPrivateServer settings{{true, "example.test", "127.1.2.3", "198.51.100.7:1-65535"}};
    CHECK(DestinationRoute({198, 51, 100, 7}, 1) == PrivateServerRoute::Routed);
    CHECK(DestinationRoute({198, 51, 100, 7}, 65535) == PrivateServerRoute::Routed);
    CHECK(DestinationRoute({127, 1, 2, 3}, 80) == PrivateServerRoute::Routed);
    CHECK(Network::RoutePrivateServerHost("example.test").gateway ==
          Network::IPv4Address{127, 1, 2, 3});
}

TEST_CASE("Network::PrivateServerMalformedSettings", "[core][network]") {
    const auto check_misconfigured = [](const char* domains, const char* gateway,
                                        const char* relay, std::string_view setting) {
        INFO("domains=\"" << domains << "\" gateway=\"" << gateway << "\" relay=\"" << relay
                          << '"');
        const ScopedPrivateServer settings{{true, domains, gateway, relay}};
        const Network::PrivateServerRouting routing = Network::RoutePrivateServerHost("example.test");
        CHECK(routing.route == Network::PrivateServerRoute::Misconfigured);
        CHECK(routing.malformed_setting == setting);
        CHECK(HostRoute("other.test") == Network::PrivateServerRoute::Misconfigured);
        CHECK(DestinationRoute({127, 0, 0, 1}, 443) == Network::PrivateServerRoute::Misconfigured);
        CHECK(DestinationRoute({203, 0, 113, 1}, 3478) ==
              Network::PrivateServerRoute::Misconfigured);
    };

    const std::string long_label_domain = std::string(64, 'a') + ".test";
    for (const char* domains :
         {"", ",", "example.test,", ",example.test", "example..test", "example.test.", ".test",
          "-example.test", "example-.test", "exa mple.test", " example.test", "*.example.test",
          "exa_mple.test", "example.test,,game.test", long_label_domain.c_str()}) {
        check_misconfigured(domains, Gateway, Relay, "private_server_domains");
    }
    for (const char* gateway : {"", "10.0.0.1", "128.0.0.1", "127.0.0", "127.0.0.1.1",
                                "127.0.0.01", "127.0.0.256", "127.0.0.1:80", " 127.0.0.1",
                                "127.0.0.1 ", "localhost", "127.0.0.-1"}) {
        check_misconfigured(Domains, gateway, Relay, "private_server_gateway");
    }
    for (const char* relay :
         {"203.0.113.1", "203.0.113.1:", "203.0.113.1:0", "203.0.113.1:65536",
          "203.0.113.1:49233-49170", "203.0.113.1:3478,", "203.0.113.1:,3478", "203.0.113.1:03478",
          "203.0.113.1:1-2-3", "203.0.113.1:-3478", "203.0.113.1:3478-", "203.0.113.1: 3478",
          "203.0.113.1:3478:3479", "203.0.113:3478", "0.0.0.0:3478", "255.255.255.255:3478",
          "224.0.0.1:3478", "239.255.255.255:3478", "relay.example.test:3478"}) {
        check_misconfigured(Domains, Gateway, relay, "private_server_relay");
    }
}

TEST_CASE("Network::PrivateServerGatewayPort", "[core][network]") {
    Network::NetworkInstance network_instance;
    Network::Socket listener;
    REQUIRE(listener.Initialize(Network::Domain::INET, Network::Type::STREAM,
                                Network::Protocol::TCP) == Network::Errno::SUCCESS);
    REQUIRE(listener.Bind({Network::Domain::INET, {127, 0, 0, 1}, 0}) ==
            Network::Errno::SUCCESS);
    REQUIRE(listener.Listen(1) == Network::Errno::SUCCESS);
    const auto [address, error] = listener.GetSockName();
    REQUIRE(error == Network::Errno::SUCCESS);

    const ScopedPrivateServer settings{{true, Domains, Gateway, Relay}};
    const auto old_port = Settings::values.private_server_gateway_port.GetValue();
    Settings::values.private_server_gateway_port.SetValue(address.portno);
    Network::Socket client;
    const auto initialized = client.Initialize(Network::Domain::INET, Network::Type::STREAM,
                                               Network::Protocol::TCP);
    const auto connected = initialized == Network::Errno::SUCCESS
                               ? client.Connect({Network::Domain::INET, {127, 0, 0, 1}, 443})
                               : initialized;
    Settings::values.private_server_gateway_port.SetValue(old_port);
    REQUIRE(initialized == Network::Errno::SUCCESS);
    REQUIRE(connected == Network::Errno::SUCCESS);
    const auto [peer, peer_error] = client.GetPeerName();
    CHECK(peer_error == Network::Errno::SUCCESS);
    CHECK(peer.portno == address.portno);
}

TEST_CASE("Network::PrivateServerSocketEgress", "[core][sockets]") {
    Network::NetworkInstance network_instance;
    const Network::SockAddrIn blocked{Network::Domain::INET, {192, 0, 2, 1}, 443};
    const Network::SockAddrIn relay_other_port{Network::Domain::INET, {203, 0, 113, 1}, 443};
    const std::array<u8, 1> data{'x'};

    Network::Socket receiver;
    REQUIRE(receiver.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                                Network::Protocol::UDP) == Network::Errno::SUCCESS);
    REQUIRE(receiver.Bind({Network::Domain::INET, {127, 0, 0, 1}, 0}) ==
            Network::Errno::SUCCESS);
    REQUIRE(receiver.SetRcvTimeo(2000) == Network::Errno::SUCCESS);
    const auto [receiver_address, receiver_error] = receiver.GetSockName();
    REQUIRE(receiver_error == Network::Errno::SUCCESS);

    Network::Socket sender;
    REQUIRE(sender.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::UDP) == Network::Errno::SUCCESS);
    Network::Socket stream;
    REQUIRE(stream.Initialize(Network::Domain::INET, Network::Type::STREAM,
                              Network::Protocol::TCP) == Network::Errno::SUCCESS);

    SECTION("The gateway is reachable, everything else is not") {
        const ScopedPrivateServer settings{{true, Domains, Gateway, Relay}};
        CHECK(sender.SendTo(0, data, &receiver_address) ==
              std::pair<s32, Network::Errno>{1, Network::Errno::SUCCESS});
        std::array<u8, 1> received{};
        CHECK(receiver.RecvFrom(0, received, nullptr) ==
              std::pair<s32, Network::Errno>{1, Network::Errno::SUCCESS});
        CHECK(sender.SendTo(0, data, &blocked) ==
              std::pair<s32, Network::Errno>{-1, Network::Errno::HOSTUNREACH});
        CHECK(sender.SendTo(0, data, &relay_other_port) ==
              std::pair<s32, Network::Errno>{-1, Network::Errno::HOSTUNREACH});
        CHECK(stream.Connect(blocked) == Network::Errno::HOSTUNREACH);
        CHECK(stream.Connect(relay_other_port) == Network::Errno::HOSTUNREACH);
        // Allowed through to the host, which refuses: nothing listens on port 1.
        CHECK(stream.Connect({Network::Domain::INET, {127, 0, 0, 1}, 1}) ==
              Network::Errno::CONNREFUSED);
    }

    SECTION("Loopback beyond the gateway is blocked, except a socket connecting to itself") {
        const ScopedPrivateServer settings{{true, Domains, "127.0.0.2", Relay}};
        CHECK(sender.SendTo(0, data, &receiver_address) ==
              std::pair<s32, Network::Errno>{-1, Network::Errno::HOSTUNREACH});
        CHECK(sender.Connect(receiver_address) == Network::Errno::HOSTUNREACH);
        CHECK(receiver.Connect(receiver_address) == Network::Errno::SUCCESS);
    }

    SECTION("Malformed settings block everything") {
        const ScopedPrivateServer settings{{true, Domains, "", Relay}};
        CHECK(sender.SendTo(0, data, &receiver_address) ==
              std::pair<s32, Network::Errno>{-1, Network::Errno::NETUNREACH});
        CHECK(stream.Connect({Network::Domain::INET, {127, 0, 0, 1}, 1}) ==
              Network::Errno::NETUNREACH);
        CHECK(receiver.Connect(receiver_address) == Network::Errno::SUCCESS);
    }
}
