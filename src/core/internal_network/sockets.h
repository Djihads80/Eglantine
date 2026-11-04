// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

#include "common/common_types.h"
#include "core/internal_network/network.h"

// TODO: C++20 Replace std::vector usages with std::span

namespace Network {

struct ProxyPacket;

class SocketBase {
public:
#ifndef _WIN32
    using SOCKET = int;
    static constexpr SOCKET INVALID_SOCKET = -1;
    static constexpr SOCKET SOCKET_ERROR = -1;
#endif

    struct AcceptResult {
        std::unique_ptr<SocketBase> socket;
        SockAddrIn sockaddr_in;
    };

    SocketBase() = default;
    explicit SocketBase(SOCKET fd_) : fd{fd_} {}
    virtual ~SocketBase() = default;

    YUZU_NON_COPYABLE(SocketBase);
    YUZU_NON_MOVEABLE(SocketBase);

    virtual Errno Initialize(Domain domain, Type type, Protocol protocol) = 0;

    virtual Errno Close() = 0;

    virtual std::pair<AcceptResult, Errno> Accept() = 0;

    virtual Errno Connect(SockAddrIn addr_in) = 0;

    virtual std::pair<SockAddrIn, Errno> GetPeerName() = 0;

    virtual std::pair<SockAddrIn, Errno> GetSockName() = 0;

    virtual Errno Bind(SockAddrIn addr) = 0;

    virtual Errno Listen(s32 backlog) = 0;

    virtual Errno Shutdown(ShutdownHow how) = 0;

    // A receive or send given FLAG_MSG_DONTWAIT in flags does not wait, whatever the mode
    // SetNonBlock gave the socket, and leaves that mode alone: calls on the socket from other
    // threads still wait or not as it has it.

    virtual std::pair<s32, Errno> Recv(int flags, std::span<u8> message) = 0;

    virtual std::pair<s32, Errno> RecvFrom(int flags, std::span<u8> message, SockAddrIn* addr) = 0;

    /// Receives one message as recvmsg does, with its source address in addr unless that is null.
    /// flags holds no more than FLAG_MSG_PEEK and FLAG_MSG_DONTWAIT. msg_flags gets
    /// FLAG_MSG_TRUNC when the part of a datagram past message was discarded (or, peeking, left
    /// unread).
    virtual std::pair<s32, Errno> RecvMsg(int flags, std::span<u8> message, SockAddrIn* addr,
                                          u32& msg_flags) = 0;

    virtual std::pair<s32, Errno> Send(std::span<const u8> message, int flags) = 0;

    virtual std::pair<s32, Errno> SendTo(u32 flags, std::span<const u8> message,
                                         const SockAddrIn* addr) = 0;

    virtual Errno SetLinger(bool enable, u32 linger) = 0;

    virtual Errno SetReuseAddr(bool enable) = 0;

    virtual Errno SetKeepAlive(bool enable) = 0;

    virtual Errno SetBroadcast(bool enable) = 0;

    virtual Errno SetSndBuf(u32 value) = 0;

    virtual Errno SetRcvBuf(u32 value) = 0;

    virtual Errno SetSndTimeo(u32 value) = 0;

    virtual Errno SetRcvTimeo(u32 value) = 0;

    virtual Errno SetNoDelay(bool enable) = 0;

    virtual Errno SetNonBlock(bool enable) = 0;

    virtual std::pair<Errno, Errno> GetPendingError() = 0;

    virtual std::pair<bool, Errno> GetReuseAddr() = 0;

    virtual std::pair<bool, Errno> GetNoDelay() = 0;

    virtual bool IsOpened() const = 0;

    /// Whether HandleProxyPacket would queue the packet; checked before decompressing it
    virtual bool AcceptsProxyPacket(const ProxyPacket& packet) = 0;

    /// Queues a room packet whose data is already decompressed
    virtual void HandleProxyPacket(const ProxyPacket& packet) = 0;

    /// For a socket emulated without a host socket, the events among those asked for that it is
    /// ready for now; nullopt for a host socket, which Poll examines instead.
    virtual std::optional<PollEvents> EmulatedPoll(PollEvents events) = 0;

    [[nodiscard]] SOCKET GetFD() const {
        return fd;
    }

protected:
    SOCKET fd = INVALID_SOCKET;
};

class Socket : public SocketBase {
public:
    Socket() = default;
    explicit Socket(SOCKET fd_) : SocketBase{fd_} {}

    ~Socket() override;

    Socket(Socket&& rhs) noexcept;

    Errno Initialize(Domain domain, Type type, Protocol protocol) override;

    Errno Close() override;

    std::pair<AcceptResult, Errno> Accept() override;

    Errno Connect(SockAddrIn addr_in) override;

    std::pair<SockAddrIn, Errno> GetPeerName() override;

    std::pair<SockAddrIn, Errno> GetSockName() override;

    Errno Bind(SockAddrIn addr) override;

    Errno Listen(s32 backlog) override;

    Errno Shutdown(ShutdownHow how) override;

    std::pair<s32, Errno> Recv(int flags, std::span<u8> message) override;

    std::pair<s32, Errno> RecvFrom(int flags, std::span<u8> message, SockAddrIn* addr) override;

    std::pair<s32, Errno> RecvMsg(int flags, std::span<u8> message, SockAddrIn* addr,
                                  u32& msg_flags) override;

    std::pair<s32, Errno> Send(std::span<const u8> message, int flags) override;

    std::pair<s32, Errno> SendTo(u32 flags, std::span<const u8> message,
                                 const SockAddrIn* addr) override;

    Errno SetLinger(bool enable, u32 linger) override;

    Errno SetReuseAddr(bool enable) override;

    Errno SetKeepAlive(bool enable) override;

    Errno SetBroadcast(bool enable) override;

    Errno SetSndBuf(u32 value) override;

    Errno SetRcvBuf(u32 value) override;

    Errno SetSndTimeo(u32 value) override;

    Errno SetRcvTimeo(u32 value) override;

    Errno SetNoDelay(bool enable) override;

    Errno SetNonBlock(bool enable) override;

    template <typename T>
    Errno SetSockOpt(SOCKET fd, int level, int option, T value);

    std::pair<Errno, Errno> GetPendingError() override;

    std::pair<bool, Errno> GetReuseAddr() override;

    std::pair<bool, Errno> GetNoDelay() override;

    template <typename T>
    std::pair<T, Errno> GetSockOpt(SOCKET fd, int level, int option);

    bool IsOpened() const override;

    bool AcceptsProxyPacket(const ProxyPacket& packet) override;

    void HandleProxyPacket(const ProxyPacket& packet) override;

    std::optional<PollEvents> EmulatedPoll(PollEvents events) override;

private:
    /// Runs operation, a host receive or send given the host flags to add to its own: those that
    /// keep it from waiting if flags holds FLAG_MSG_DONTWAIT, none otherwise.
    template <typename Operation>
    std::pair<s32, Errno> Transfer(int flags, Operation operation);

#ifdef _WIN32
    // Winsock has no MSG_DONTWAIT, so a call given FLAG_MSG_DONTWAIT makes the host socket
    // nonblocking while it runs. It stays nonblocking while any such call or the socket's mode
    // needs it, so that no such call ever waits and none undoes SetNonBlock. A blocking call that
    // failed with EAGAIN only because such a call ran meanwhile is retried once none runs.

    /// Makes the host socket nonblocking for a call given FLAG_MSG_DONTWAIT
    Errno BeginDontWait();
    /// Gives the host socket back the socket's mode once no call given FLAG_MSG_DONTWAIT runs
    void EndDontWait();
    /// Whether a blocking call that failed with EAGAIN, having started at dont_wait_epoch epoch,
    /// only did because calls given FLAG_MSG_DONTWAIT made the host socket nonblocking; if so,
    /// waits for the host socket to block again, for the call to be retried.
    bool WaitOutDontWait(u64 epoch);

    /// Guards the host socket's mode, is_non_blocking and dont_wait_calls.
    std::mutex mode_mutex;
    /// Notified when the host socket stops being nonblocking for calls given FLAG_MSG_DONTWAIT
    /// alone, or the socket becomes nonblocking.
    std::condition_variable dont_wait_ended;
    /// Calls given FLAG_MSG_DONTWAIT that are running.
    u32 dont_wait_calls = 0;
    /// Bumped each time the host socket starts or stops being nonblocking only for calls given
    /// FLAG_MSG_DONTWAIT, the socket's mode being blocking; odd while it is.
    std::atomic<u64> dont_wait_epoch{0};
#endif

    /// The mode SetNonBlock gave the socket.
    std::atomic_bool is_non_blocking{false};
};

std::pair<s32, Errno> Poll(std::vector<PollFD>& poll_fds, s32 timeout);

} // namespace Network
