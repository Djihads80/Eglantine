// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/ldn/lan_discovery.h"
#include "core/internal_network/network_interface.h"

using namespace std::chrono_literals;

namespace {

class TestLANDiscovery : public Service::LDN::LANDiscovery {
public:
    void SetConnectionTimeout(std::chrono::nanoseconds timeout) {
        connection_timeout = timeout;
    }

    std::chrono::nanoseconds GetConnectionTimeout() const {
        return connection_timeout;
    }

    bool WaitUntilConnecting() {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        do {
            {
                std::scoped_lock lock{packet_mutex};
                if (host_ip) {
                    // Connect has published its target and released this lock to wait for a reply.
                    return true;
                }
            }
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }
};

} // namespace

TEST_CASE("LDN waits for the selected host's connection reply", "[core][ldn]") {
    using namespace Service::LDN;
    if (!Network::GetSelectedNetworkInterface()) {
        SKIP("LDN requires a host IPv4 interface");
    }

    TestLANDiscovery station;
    REQUIRE(station.Initialize([] {}).IsSuccess());
    REQUIRE(station.OpenStation().IsSuccess());

    NetworkInfo info{};
    info.network_id.session_id = {1, 2};
    info.ldn.node_count = 1;
    info.ldn.node_count_max = 8;
    info.ldn.nodes[0].ipv4_address = {1, 0, 168, 192};

    Network::LDNPacket reply{};
    reply.type = Network::LDNPacketType::SyncNetwork;
    reply.local_ip = {192, 168, 0, 1};
    reply.data.resize(sizeof(info));
    std::memcpy(reply.data.data(), &info, sizeof(info));

    // A scan result or unsolicited sync must not connect an idle station.
    station.ReceivePacket(reply);
    REQUIRE(station.GetState() == State::StationOpened);

    auto connecting = std::async(std::launch::async, [&] { return station.Connect(info, {}, 1); });
    REQUIRE(station.WaitUntilConnecting());

    auto unrelated = reply;
    unrelated.local_ip[3] = 2;
    station.ReceivePacket(unrelated);
    REQUIRE(connecting.wait_for(0ms) == std::future_status::timeout);

    auto stale = info;
    stale.network_id.session_id.low++;
    std::memcpy(unrelated.data.data(), &stale, sizeof(stale));
    unrelated.local_ip = reply.local_ip;
    station.ReceivePacket(unrelated);
    REQUIRE(connecting.wait_for(0ms) == std::future_status::timeout);

    unrelated.data.resize(1);
    station.ReceivePacket(unrelated);
    REQUIRE(connecting.wait_for(0ms) == std::future_status::timeout);

    station.ReceivePacket(reply);
    REQUIRE(connecting.wait_for(5s) == std::future_status::ready);
    REQUIRE(connecting.get().IsSuccess());
    REQUIRE(station.GetState() == State::StationConnected);
    REQUIRE(station.Finalize().IsSuccess());
}

TEST_CASE("LDN host validates join packets and does not allocate duplicate stations",
          "[core][ldn]") {
    using namespace Service::LDN;
    if (!Network::GetSelectedNetworkInterface()) {
        SKIP("LDN requires a host IPv4 interface");
    }

    LANDiscovery host;
    REQUIRE(host.Initialize([] {}).IsSuccess());
    REQUIRE(host.OpenAccessPoint().IsSuccess());
    NetworkConfig config{};
    config.node_count_max = 2;
    REQUIRE(host.CreateNetwork({}, {}, config).IsSuccess());

    NodeInfo node{};
    node.is_connected = 1;
    node.mac_address.raw[5] = 1;
    Network::LDNPacket request{};
    request.type = Network::LDNPacketType::Connect;
    request.local_ip = {192, 168, 0, 2};
    request.data.resize(sizeof(node));
    std::memcpy(request.data.data(), &node, sizeof(node));
    host.ReceivePacket(request);
    NetworkInfo info{};
    REQUIRE(host.GetNetworkInfo(info).IsSuccess());
    REQUIRE(info.ldn.node_count == 2);

    host.ReceivePacket(request);
    REQUIRE(host.GetNetworkInfo(info).IsSuccess());
    REQUIRE(info.ldn.node_count == 2);

    // A third console cannot enter a two-console network.
    request.local_ip[3] = 3;
    host.ReceivePacket(request);
    REQUIRE(host.GetNetworkInfo(info).IsSuccess());
    REQUIRE(info.ldn.node_count == 2);

    request.type = Network::LDNPacketType::Disconnect;
    request.data.resize(1);
    host.ReceivePacket(request);
    REQUIRE(host.GetNetworkInfo(info).IsSuccess());
    REQUIRE(info.ldn.node_count == 2);
    REQUIRE(host.Finalize().IsSuccess());
}

TEST_CASE("LDN reports a missing connection reply and ignores late replies", "[core][ldn]") {
    using namespace Service::LDN;
    if (!Network::GetSelectedNetworkInterface()) {
        SKIP("LDN requires a host IPv4 interface");
    }

    TestLANDiscovery station;
    REQUIRE(station.GetConnectionTimeout() == 5s);
    station.SetConnectionTimeout(25ms);
    REQUIRE(station.Initialize([] {}).IsSuccess());
    REQUIRE(station.OpenStation().IsSuccess());
    NetworkInfo info{};
    info.ldn.node_count = 1;
    info.ldn.nodes[0].ipv4_address = {1, 0, 168, 192};
    REQUIRE(station.Connect(info, {}, 1) == ResultAuthenticationTimeout);
    REQUIRE(station.GetState() == State::StationOpened);

    Network::LDNPacket reply{};
    reply.type = Network::LDNPacketType::SyncNetwork;
    reply.local_ip = {192, 168, 0, 1};
    reply.data.resize(sizeof(info));
    std::memcpy(reply.data.data(), &info, sizeof(info));
    station.ReceivePacket(reply);
    REQUIRE(station.GetState() == State::StationOpened);
    REQUIRE(station.Finalize().IsSuccess());
}

TEST_CASE("LDN creates fresh identities for recreated networks and separate hosts", "[core][ldn]") {
    using namespace Service::LDN;
    if (!Network::GetSelectedNetworkInterface()) {
        SKIP("LDN requires a host IPv4 interface");
    }

    LANDiscovery host;
    LANDiscovery other_host;
    REQUIRE(host.Initialize([] {}).IsSuccess());
    REQUIRE(other_host.Initialize([] {}).IsSuccess());
    REQUIRE(host.OpenAccessPoint().IsSuccess());
    REQUIRE(other_host.OpenAccessPoint().IsSuccess());
    NetworkConfig config{};
    config.node_count_max = 2;
    REQUIRE(host.CreateNetwork({}, {}, config).IsSuccess());
    NetworkInfo first{};
    REQUIRE(host.GetNetworkInfo(first).IsSuccess());

    REQUIRE(host.DestroyNetwork().IsSuccess());
    REQUIRE(host.CreateNetwork({}, {}, config).IsSuccess());
    NetworkInfo recreated{};
    REQUIRE(host.GetNetworkInfo(recreated).IsSuccess());
    REQUIRE(recreated.network_id.session_id != first.network_id.session_id);

    REQUIRE(other_host.CreateNetwork({}, {}, config).IsSuccess());
    NetworkInfo other{};
    REQUIRE(other_host.GetNetworkInfo(other).IsSuccess());
    REQUIRE(other.network_id.session_id != first.network_id.session_id);
    REQUIRE(other.network_id.session_id != recreated.network_id.session_id);
    REQUIRE(host.Finalize().IsSuccess());
    REQUIRE(other_host.Finalize().IsSuccess());
}
