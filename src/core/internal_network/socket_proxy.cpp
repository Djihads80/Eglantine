// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>

#include "common/assert.h"
#include "common/logging.h"
#include "common/zstd_compression.h"
#include "core/internal_network/network.h"
#include "core/internal_network/network_interface.h"
#include "core/internal_network/socket_proxy.h"
#include "network/network.h"

#ifdef __unix__
#include <sys/socket.h>
#endif

namespace Network {

namespace {

constexpr u16 EphemeralPortFirst = 49152;
constexpr u16 EphemeralPortLast = 65535;

u16 AllocateEphemeralPort() {
    static std::atomic<u32> next{EphemeralPortFirst};
    const u32 value = next.fetch_add(1, std::memory_order_relaxed);
    return static_cast<u16>(EphemeralPortFirst + (value - EphemeralPortFirst) %
                                                     (EphemeralPortLast - EphemeralPortFirst + 1));
}

bool IsAnyAddress(const IPv4Address& address) {
    return std::all_of(address.begin(), address.end(), [](u8 octet) { return octet == 0; });
}

bool IsRoomBroadcast(const IPv4Address& address) {
    return address == IPv4Address{255, 255, 255, 255} || address == IPv4Address{192, 168, 255, 255};
}

u16 InternetChecksum(std::span<const u8> data) {
    u32 sum = 0;
    for (size_t i = 0; i + 1 < data.size(); i += 2) {
        sum += static_cast<u16>((static_cast<u16>(data[i]) << 8) | data[i + 1]);
    }
    if (data.size() % 2 != 0) {
        sum += static_cast<u16>(data.back() << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return static_cast<u16>(~sum);
}

} // namespace

ProxySocket::ProxySocket() noexcept {}

ProxySocket::~ProxySocket() {
    if (fd == INVALID_SOCKET) {
        return;
    }
    fd = INVALID_SOCKET;
}

bool ProxySocket::MatchesPacket(const ProxyPacket& packet) const {
    if (protocol != packet.protocol || local_endpoint.portno != packet.remote_endpoint.portno ||
        closed) {
        return false;
    }
    if (!packet.broadcast && !IsAnyAddress(local_endpoint.ip) &&
        local_endpoint.ip != packet.remote_endpoint.ip) {
        return false;
    }
    if (peer_endpoint && (peer_endpoint->ip != packet.local_endpoint.ip ||
                          peer_endpoint->portno != packet.local_endpoint.portno)) {
        return false;
    }
    return true;
}

bool ProxySocket::AcceptsProxyPacket(const ProxyPacket& packet) {
    std::lock_guard guard(packets_mutex);
    return MatchesPacket(packet);
}

void ProxySocket::HandleProxyPacket(const ProxyPacket& packet) {
    {
        // Runs on the room thread, while the game thread may bind, close or reconfigure the socket.
        // Check again: the socket may have changed since AcceptsProxyPacket.
        std::lock_guard guard(packets_mutex);
        if (!MatchesPacket(packet)) {
            return;
        }
        received_packets.push(packet);
    }
    // A MSG_PEEK reader leaves the packet queued, so every waiter must see it
    packets_cv.notify_all();
}

std::optional<PollEvents> ProxySocket::EmulatedPoll(PollEvents events) {
    std::lock_guard guard(packets_mutex);
    PollEvents revents{};
    // A queued packet reads at once. So does a closed socket, whose reads fail, and a stream,
    // whose reads report its end while nothing is queued (see Recv).
    if (closed || !received_packets.empty() || socket_type == Type::STREAM) {
        revents |= events & (PollEvents::In | PollEvents::RdNorm);
    }
    // Sends go to the room without waiting.
    revents |= events & PollEvents::Out;
    return revents;
}

template <typename T>
Errno ProxySocket::SetSockOpt(SOCKET fd_, int option, T value) {
    LOG_DEBUG(Network, "(STUBBED) called");
    return Errno::SUCCESS;
}

Errno ProxySocket::Initialize(Domain domain, Type type, Protocol socket_protocol) {
    {
        std::lock_guard guard(packets_mutex);
        socket_type = type;
        protocol = socket_protocol == Protocol::Unspecified && type == Type::DGRAM
                       ? Protocol::UDP
                       : socket_protocol;
    }
    SetSockOpt(fd, SO_TYPE, type);

    return Errno::SUCCESS;
}

std::pair<ProxySocket::AcceptResult, Errno> ProxySocket::Accept() {
    LOG_WARNING(Network, "(STUBBED) called");
    return {AcceptResult{}, Errno::SUCCESS};
}

Errno ProxySocket::Connect(SockAddrIn addr_in) {
    std::lock_guard guard(packets_mutex);
    if (closed) {
        return Errno::BADF;
    }
    peer_endpoint = addr_in;
    return Errno::SUCCESS;
}

std::pair<SockAddrIn, Errno> ProxySocket::GetPeerName() {
    std::lock_guard guard(packets_mutex);
    if (!peer_endpoint) {
        return {SockAddrIn{}, Errno::NOTCONN};
    }
    return {*peer_endpoint, Errno::SUCCESS};
}

std::pair<SockAddrIn, Errno> ProxySocket::GetSockName() {
    std::lock_guard guard(packets_mutex);
    return {local_endpoint, Errno::SUCCESS};
}

Errno ProxySocket::Bind(SockAddrIn addr) {
    std::lock_guard guard(packets_mutex);
    if (is_bound) {
        return Errno::INVAL;
    }
    if (addr.family != Domain::INET) {
        return Errno::INVAL;
    }
    if (addr.portno == 0) {
        addr.portno = AllocateEphemeralPort();
    }
    local_endpoint = addr;
    is_bound = true;

    return Errno::SUCCESS;
}

Errno ProxySocket::Listen(s32 backlog) {
    LOG_WARNING(Network, "(STUBBED) called");
    return Errno::SUCCESS;
}

Errno ProxySocket::Shutdown(ShutdownHow how) {
    LOG_WARNING(Network, "(STUBBED) called");
    return Errno::SUCCESS;
}

std::pair<s32, Errno> ProxySocket::Recv(int flags, std::span<u8> message) {
    u32 msg_flags = 0;
    return RecvMsg(flags, message, nullptr, msg_flags);
}

std::pair<s32, Errno> ProxySocket::RecvFrom(int flags, std::span<u8> message, SockAddrIn* addr) {
    u32 msg_flags = 0;
    return Receive(flags, message, addr, msg_flags);
}

std::pair<s32, Errno> ProxySocket::RecvMsg(int flags, std::span<u8> message, SockAddrIn* addr,
                                           u32& msg_flags) {
    msg_flags = 0;
    {
        // Room TCP sockets have no peer (Connect and Send are stubs). Report an empty queue as the
        // end of the stream, as before, instead of blocking the bsd service until the timeout.
        std::lock_guard guard(packets_mutex);
        if (socket_type == Type::STREAM && received_packets.empty()) {
            return {0, Errno::SUCCESS};
        }
    }
    return Receive(flags, message, addr, msg_flags);
}

std::pair<s32, Errno> ProxySocket::Receive(int flags, std::span<u8> message, SockAddrIn* addr,
                                           u32& msg_flags) {
    ASSERT((flags & ~(FLAG_MSG_PEEK | FLAG_MSG_DONTWAIT)) == 0);
    ASSERT(message.size() < static_cast<size_t>((std::numeric_limits<int>::max)()));

    std::unique_lock lock(packets_mutex);
    if (closed) {
        return {-1, Errno::BADF};
    }
    if (received_packets.empty()) {
        if (!blocking || (flags & FLAG_MSG_DONTWAIT) != 0) {
            return {-1, Errno::AGAIN};
        }
        // TODO (flTobi): Verify the timeout behavior and break when connection is lost
        // When receive_timeout is set to zero, the socket is supposed to wait indefinitely until a
        // packet arrives. In order to prevent lost packets from hanging the emulation thread, we
        // set the timeout to 5s instead. An expired wait is reported like an SO_RCVTIMEO timeout.
        const u32 timeout = receive_timeout == 0 ? 5000 : receive_timeout;
        if (!packets_cv.wait_for(lock, std::chrono::milliseconds{timeout},
                                 [this] { return closed || !received_packets.empty(); })) {
            return {-1, Errno::AGAIN};
        }
        if (closed) {
            return {-1, Errno::BADF};
        }
    }

    return ReceivePacket(flags, message, addr, message.size(), msg_flags);
}

std::pair<s32, Errno> ProxySocket::ReceivePacket(int flags, std::span<u8> message, SockAddrIn* addr,
                                                 std::size_t max_length, u32& msg_flags) {
    const ProxyPacket& packet = received_packets.front();
    if (addr) {
        addr->family = Domain::INET;
        addr->ip = packet.local_endpoint.ip;         // The senders ip address
        addr->portno = packet.local_endpoint.portno; // The senders port number
    }

    const std::span<const u8> unread = std::span{packet.data}.subspan(front_read_offset);
    const std::size_t read_bytes = (std::min)(unread.size(), max_length);
    std::memcpy(message.data(), unread.data(), read_bytes);

    // Unlike a stream's, the part of a datagram that does not fit is never read: report it.
    if (socket_type != Type::STREAM && read_bytes < unread.size()) {
        msg_flags |= FLAG_MSG_TRUNC;
    }

    if ((flags & FLAG_MSG_PEEK) != 0) {
        return {static_cast<s32>(read_bytes), Errno::SUCCESS};
    }

    if (socket_type == Type::STREAM && read_bytes < unread.size()) {
        // Keep the unread part of the stream for the next call
        front_read_offset += read_bytes;
    } else {
        // Like BSD recv(), the rest of a datagram that does not fit is discarded
        received_packets.pop();
        front_read_offset = 0;
    }

    return {static_cast<s32>(read_bytes), Errno::SUCCESS};
}

std::pair<s32, Errno> ProxySocket::Send(std::span<const u8> message, int flags) {
    ASSERT(message.size() < static_cast<size_t>((std::numeric_limits<int>::max)()));
    ASSERT((flags & ~FLAG_MSG_DONTWAIT) == 0);

    std::optional<SockAddrIn> peer;
    {
        std::lock_guard guard(packets_mutex);
        peer = peer_endpoint;
    }
    if (!peer) {
        return {-1, Errno::NOTCONN};
    }
    return SendTo(static_cast<u32>(flags), message, &*peer);
}

bool ProxySocket::SendPacket(ProxyPacket& packet) {
    if (auto room_member = Network::GetRoomMember().lock()) {
        if (Network::GetRoomIPv4Address()) {
            packet.data = Common::Compression::CompressDataZSTDDefault(packet.data.data(),
                                                                       packet.data.size());
            room_member->SendProxyPacket(packet);
            return true;
        }
    }
    return false;
}

std::pair<s32, Errno> ProxySocket::SendTo(u32 flags, std::span<const u8> message,
                                          const SockAddrIn* addr) {
    ASSERT((flags & ~FLAG_MSG_DONTWAIT) == 0);

    if (addr == nullptr) {
        return {-1, Errno::NOTCONN};
    }

    Type current_type;
    Protocol current_protocol;
    SockAddrIn current_local_endpoint;
    bool current_broadcast;
    {
        std::lock_guard guard(packets_mutex);
        if (closed) {
            return {-1, Errno::BADF};
        }
    if (!is_bound) {
            local_endpoint = {
                .family = Domain::INET,
                .ip = {},
                .portno = AllocateEphemeralPort(),
            };
            is_bound = true;
        }
        current_type = socket_type;
        current_protocol = protocol;
        current_local_endpoint = local_endpoint;
        current_broadcast = broadcast;
    }

    if (current_type == Type::RAW && current_protocol == Protocol::ICMP) {
        if (message.size() < 8 || message[0] != 8) {
            return {-1, Errno::INVAL};
        }

        ProxyPacket reply{};
        reply.local_endpoint = *addr;
        reply.remote_endpoint = current_local_endpoint;
        reply.protocol = Protocol::ICMP;
        reply.data.assign(message.begin(), message.end());
        reply.data[0] = 0;
        reply.data[2] = 0;
        reply.data[3] = 0;
        const u16 checksum = InternetChecksum(reply.data);
        reply.data[2] = static_cast<u8>(checksum >> 8);
        reply.data[3] = static_cast<u8>(checksum);

        {
            std::lock_guard guard(packets_mutex);
            received_packets.push(std::move(reply));
        }
        packets_cv.notify_all();
            return {static_cast<s32>(message.size()), Errno::SUCCESS};
        }

    const auto room_ip = Network::GetRoomIPv4Address();
    if (!room_ip) {
        return {-1, Errno::NETDOWN};
    }

    ProxyPacket packet;
    packet.local_endpoint = current_local_endpoint;
    packet.remote_endpoint = *addr;
    packet.protocol = current_protocol;
    packet.broadcast = IsRoomBroadcast(packet.remote_endpoint.ip);
    if (packet.broadcast && !current_broadcast) {
        return {-1, Errno::INVAL};
    }
    packet.local_endpoint.ip = *room_ip;

    packet.data.clear();
    std::copy(message.begin(), message.end(), std::back_inserter(packet.data));

    if (!SendPacket(packet)) {
        return {-1, Errno::NETDOWN};
    }

    return {static_cast<s32>(message.size()), Errno::SUCCESS};
}

Errno ProxySocket::Close() {
    {
        std::lock_guard guard(packets_mutex);
        fd = INVALID_SOCKET;
        closed = true;
        received_packets = {};
        front_read_offset = 0;
    }
    packets_cv.notify_all();

    return Errno::SUCCESS;
}

Errno ProxySocket::SetLinger(bool enable, u32 linger) {
    struct Linger {
        u16 linger_enable;
        u16 linger_time;
    } values;
    values.linger_enable = enable ? 1 : 0;
    values.linger_time = static_cast<u16>(linger);

    return SetSockOpt(fd, SO_LINGER, values);
}

Errno ProxySocket::SetReuseAddr(bool enable) {
    {
        std::lock_guard guard(packets_mutex);
        reuse_addr = enable;
    }
    return SetSockOpt<u32>(fd, SO_REUSEADDR, enable ? 1 : 0);
}

Errno ProxySocket::SetReusePort(bool enable) {
    std::lock_guard guard(packets_mutex);
    reuse_port = enable;
    return Errno::SUCCESS;
}

Errno ProxySocket::SetBroadcast(bool enable) {
    {
        std::lock_guard guard(packets_mutex);
        broadcast = enable;
    }
    return SetSockOpt<u32>(fd, SO_BROADCAST, enable ? 1 : 0);
}

Errno ProxySocket::SetSndBuf(u32 value) {
    return SetSockOpt(fd, SO_SNDBUF, value);
}

Errno ProxySocket::SetKeepAlive(bool enable) {
    return Errno::SUCCESS;
}

Errno ProxySocket::SetRcvBuf(u32 value) {
    return SetSockOpt(fd, SO_RCVBUF, value);
}

Errno ProxySocket::SetSndTimeo(u32 value) {
    send_timeout = value;
    return SetSockOpt(fd, SO_SNDTIMEO, static_cast<int>(value));
}

Errno ProxySocket::SetRcvTimeo(u32 value) {
    {
        std::lock_guard guard(packets_mutex);
        receive_timeout = value;
    }
    return SetSockOpt(fd, SO_RCVTIMEO, static_cast<int>(value));
}

Errno ProxySocket::SetNoDelay(bool enable) {
    // Room sockets have no TCP stack; the option is only kept for GetNoDelay.
    std::lock_guard guard(packets_mutex);
    no_delay = enable;
    return Errno::SUCCESS;
}

Errno ProxySocket::SetNonBlock(bool enable) {
    std::lock_guard guard(packets_mutex);
    blocking = !enable;
    return Errno::SUCCESS;
}

std::pair<Errno, Errno> ProxySocket::GetPendingError() {
    LOG_DEBUG(Network, "(STUBBED) called");
    return {Errno::SUCCESS, Errno::SUCCESS};
}

std::pair<bool, Errno> ProxySocket::GetReuseAddr() {
    std::lock_guard guard(packets_mutex);
    return {reuse_addr, Errno::SUCCESS};
}

std::pair<bool, Errno> ProxySocket::GetNoDelay() {
    std::lock_guard guard(packets_mutex);
    return {no_delay, Errno::SUCCESS};
}

bool ProxySocket::IsOpened() const {
    return fd != INVALID_SOCKET;
}

} // namespace Network
