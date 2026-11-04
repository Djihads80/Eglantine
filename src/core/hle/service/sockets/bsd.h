// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>
#include <variant>

#include "common/common_types.h"
#include "common/socket_types.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sockets/sockets.h"
#include "network/network.h"

namespace Core {
class System;
}

namespace Service {
class ServerManager;
}

namespace Network {
class SocketBase;
class Socket;
} // namespace Network

namespace Service::Sockets {

class BSD_USA final : public ServiceFramework<BSD_USA> {
public:
    /// Retries the Selects, Polls and eventfd reads and writes that wait, deferred, on the bsd
    /// services of one server manager.
    class SelectNotifier;

    /// Creates the notifier for a server manager, which manages its deferral: the notifier signals
    /// the deferral event, and forgets the deferred requests the server manager retires. To be
    /// called before the server manager registers services or starts threads.
    static std::shared_ptr<SelectNotifier> CreateSelectNotifier(Core::System& system,
                                                                ServerManager& server_manager);

    explicit BSD_USA(Core::System& system_, const char* name, bool is_user,
                     std::shared_ptr<SelectNotifier> select_notifier);
    ~BSD_USA() override;

    // These methods are called from SSL; the first two are also called from
    // this class for the corresponding IPC methods.
    // On the real device, the SSL service makes IPC calls to this service.
    std::variant<s32, Errno> DuplicateSocketImpl(s32 fd);
    Errno CloseImpl(s32 fd);
    std::optional<std::shared_ptr<Network::SocketBase>> GetSocket(s32 fd);

private:
    /// Maximum number of file descriptors
    static constexpr size_t MAX_FD = 128;
    /// Largest decompressed room packet. A UDP datagram cannot be larger.
    static constexpr size_t MAX_ROOM_PACKET_SIZE = 0x10000;

    /// Counter behind a descriptor created by EventFd.
    struct EventFdState;

    /// A descriptor is open while it has a socket that no duplicate closed, or an eventfd.
    struct FileDescriptor {
        /// Shared with the descriptor's duplicates; null for an eventfd.
        std::shared_ptr<Network::SocketBase> socket;
        /// Shared with the descriptor's duplicates, which keep it open; null for a socket.
        std::shared_ptr<EventFdState> event_fd;
        /// File status flags of a socket. An eventfd keeps its own, shared by its duplicates.
        s32 flags = 0;
        bool is_connection_based = false;
        /// Set once a duplicate sharing the socket was closed, which closed the socket.
        bool socket_closed = false;
        /// The socket's SO_RCVTIMEO and SO_SNDTIMEO in milliseconds, 0 for none: how long a
        /// deferred receive or send waits. Set on every duplicate sharing the socket.
        u32 receive_timeout = 0;
        u32 send_timeout = 0;
    };

    /// What a deferred Select, Poll, RecvMMsg, SendMMsg, Read or Write goes on with when retried.
    struct SelectRequest;
    /// What a deferred Select, Poll, RecvMMsg, SendMMsg, Read or Write waits on to be retried.
    struct SelectWait;

    struct AcceptWork {
        void Execute(BSD_USA* bsd);
        void Response(HLERequestContext& ctx);

        s32 fd;
        std::vector<u8> write_buffer;
        s32 ret{};
        Errno bsd_errno{};
    };

    struct ConnectWork {
        void Execute(BSD_USA* bsd);
        void Response(HLERequestContext& ctx);

        s32 fd;
        std::span<const u8> addr;
        Errno bsd_errno{};
    };

    struct RecvWork {
        void Execute(BSD_USA* bsd);
        void Response(HLERequestContext& ctx);

        s32 fd;
        u32 flags;
        std::vector<u8> message;
        s32 ret{};
        Errno bsd_errno{};
    };

    struct RecvFromWork {
        void Execute(BSD_USA* bsd);
        void Response(HLERequestContext& ctx);

        s32 fd;
        u32 flags;
        std::vector<u8> message;
        std::vector<u8> addr;
        s32 ret{};
        Errno bsd_errno{};
    };

    struct SendWork {
        void Execute(BSD_USA* bsd);
        void Response(HLERequestContext& ctx);

        s32 fd;
        u32 flags;
        std::span<const u8> message;
        s32 ret{};
        Errno bsd_errno{};
    };

    struct SendToWork {
        void Execute(BSD_USA* bsd);
        void Response(HLERequestContext& ctx);

        s32 fd;
        u32 flags;
        std::span<const u8> message;
        std::span<const u8> addr;
        s32 ret{};
        Errno bsd_errno{};
    };

    void RegisterClient(HLERequestContext& ctx);
    void StartMonitoring(HLERequestContext& ctx);
    void Socket(HLERequestContext& ctx);
    void SocketExempt(HLERequestContext& ctx);
    void Select(HLERequestContext& ctx);
    void Poll(HLERequestContext& ctx);
    void Accept(HLERequestContext& ctx);
    void Bind(HLERequestContext& ctx);
    void Connect(HLERequestContext& ctx);
    void GetPeerName(HLERequestContext& ctx);
    void GetSockName(HLERequestContext& ctx);
    void GetSockOpt(HLERequestContext& ctx);
    void Listen(HLERequestContext& ctx);
    void Fcntl(HLERequestContext& ctx);
    void SetSockOpt(HLERequestContext& ctx);
    void Shutdown(HLERequestContext& ctx);
    void Recv(HLERequestContext& ctx);
    void RecvFrom(HLERequestContext& ctx);
    void Send(HLERequestContext& ctx);
    void SendTo(HLERequestContext& ctx);
    void RecvMMsg(HLERequestContext& ctx);
    void SendMMsg(HLERequestContext& ctx);
    void Write(HLERequestContext& ctx);
    void Read(HLERequestContext& ctx);
    void Close(HLERequestContext& ctx);
    void DuplicateSocket(HLERequestContext& ctx);
    void EventFd(HLERequestContext& ctx);

    template <typename Work>
    void ExecuteWork(HLERequestContext& ctx, Work work);

    std::pair<s32, Errno> SocketImpl(Domain domain, Type type, Protocol protocol);
    /// Examines the pollfds once without waiting. While none has an event and the deadline is
    /// ahead, defers ctx, to be retried once one may have, and returns nullopt. On success,
    /// write_buffer holds the pollfds to write back.
    std::optional<std::pair<s32, Errno>> PollImpl(HLERequestContext& ctx,
                                                  std::vector<u8>& write_buffer, s32 nfds,
                                                  s32 timeout);
    /// Examines the descriptors of a Select once without waiting. Returns nullopt while none is
    /// ready and the deadline is ahead, having filled wait with what may change that; otherwise
    /// ready_sets gets the sets to write back.
    std::optional<std::pair<s32, Errno>> SelectImpl(const SelectRequest& request,
                                                    std::array<std::vector<u8>, 3>& ready_sets,
                                                    SelectWait& wait);
    std::pair<s32, Errno> AcceptImpl(s32 fd, std::vector<u8>& write_buffer);
    Errno BindImpl(s32 fd, std::span<const u8> addr);
    Errno ConnectImpl(s32 fd, std::span<const u8> addr);
    Errno GetPeerNameImpl(s32 fd, std::vector<u8>& write_buffer);
    Errno GetSockNameImpl(s32 fd, std::vector<u8>& write_buffer);
    Errno ListenImpl(s32 fd, s32 backlog);
    std::pair<s32, Errno> FcntlImpl(s32 fd, FcntlCmd cmd, s32 arg);
    Errno GetSockOptImpl(s32 fd, u32 level, OptName optname, std::vector<u8>& optval);
    Errno SetSockOptImpl(s32 fd, u32 level, OptName optname, std::span<const u8> optval);
    Errno ShutdownImpl(s32 fd, s32 how);
    std::pair<s32, Errno> RecvImpl(s32 fd, u32 flags, std::vector<u8>& message);
    std::pair<s32, Errno> RecvFromImpl(s32 fd, u32 flags, std::vector<u8>& message,
                                       std::vector<u8>& addr);
    std::pair<s32, Errno> SendImpl(s32 fd, u32 flags, std::span<const u8> message);
    std::pair<s32, Errno> SendToImpl(s32 fd, u32 flags, std::span<const u8> message,
                                     std::span<const u8> addr);
    /// Receives the messages serialized in messages for ctx, which then holds the reply. Returns
    /// nullopt when a blocking receive has to wait, having deferred ctx instead.
    std::optional<std::pair<s32, Errno>> RecvMMsgImpl(HLERequestContext& ctx, s32 fd, u32 vlen,
                                                      u32 flags, s64 timeout_seconds,
                                                      s64 timeout_nanoseconds,
                                                      std::vector<u8>& messages);
    /// Sends the messages serialized in messages for ctx, filling in their msg_len. Returns
    /// nullopt when a blocking send has to wait, having deferred ctx instead.
    std::optional<std::pair<s32, Errno>> SendMMsgImpl(HLERequestContext& ctx, s32 fd, u32 vlen,
                                                      u32 flags, std::vector<u8>& messages);
    /// Reads fd for ctx. Returns nullopt when it has to wait on a blocking eventfd, having
    /// deferred ctx instead.
    std::optional<std::pair<s32, Errno>> ReadImpl(HLERequestContext& ctx, s32 fd,
                                                  std::vector<u8>& message);
    /// Writes fd for ctx. Returns nullopt when it has to wait on a blocking eventfd, having
    /// deferred ctx instead.
    std::optional<std::pair<s32, Errno>> WriteImpl(HLERequestContext& ctx, s32 fd,
                                                   std::span<const u8> message);
    /// Runs operation, a read or write of the eventfd of descriptor, found at fd in a slot of the
    /// given generation. One that would block defers ctx, to be retried once the counter changes
    /// or the descriptor closes, and returns nullopt.
    template <typename Operation>
    std::optional<std::pair<s32, Errno>> EventFdIoImpl(HLERequestContext& ctx, s32 fd,
                                                       const FileDescriptor& descriptor,
                                                       u64 generation, Operation operation);
    std::pair<s32, Errno> EventFdImpl(u64 initval, u32 flags);

    /// The first free descriptor handle, or -1. The caller holds file_descriptors_mutex.
    s32 FindFreeFileDescriptorHandle() noexcept;
    /// Whether fd has a slot in use. The caller holds file_descriptors_mutex.
    bool IsFileDescriptorValid(s32 fd) const noexcept;
    /// The open descriptor at fd, or null. The caller holds file_descriptors_mutex.
    static const FileDescriptor* FindOpenDescriptor(s32 fd) noexcept;
    /// Copies the open descriptor at fd under file_descriptors_mutex, so that its socket or
    /// eventfd outlives a concurrent close; nullopt if fd is not open. Given generation, sets it
    /// to the generation of the slot.
    std::optional<FileDescriptor> CopyFileDescriptor(s32 fd, u64* generation = nullptr);
    /// CopyFileDescriptor for a socket operation; nullopt also if fd is an eventfd
    std::optional<FileDescriptor> CopyDescriptor(s32 fd);
    /// Whether the socket of descriptor at fd is blocking, from the flags fd has now. A receive
    /// or send tried with FLAG_MSG_DONTWAIT so as not to wait leaves an EAGAIN of a blocking
    /// socket to its caller to wait out.
    static bool IsBlocking(s32 fd, const FileDescriptor& descriptor);
    /// Defers ctx, rather than wait in a thread of the service, until the socket of descriptor at
    /// fd, in a slot of the given generation, may be ready to receive or, given send, to send.
    /// It is retried then, once request.deadline passes, fd closes, or emulation stops; the
    /// retry is to call Take and check the generation. Returns false, having deferred nothing,
    /// once requests can no longer wait.
    bool DeferSocketWait(HLERequestContext& ctx, s32 fd, const FileDescriptor& descriptor,
                         u64 generation, bool send, SelectRequest& request);

    void BuildErrnoResponse(HLERequestContext& ctx, Errno bsd_errno) const noexcept;

    static inline std::array<std::optional<FileDescriptor>, MAX_FD> file_descriptors{};
    /// Per slot of file_descriptors, bumped whenever its descriptor closes, so that a request
    /// deferred on a descriptor tells it from a later one in the same slot.
    static inline std::array<u64, MAX_FD> file_descriptor_generations{};
    /// Guards file_descriptors and file_descriptor_generations. Calls copy descriptors out
    /// (CopyFileDescriptor), so sockets outlive a concurrent close. Never held across a call that
    /// can block.
    static inline std::mutex file_descriptors_mutex;

    std::shared_ptr<SelectNotifier> select_notifier;

    /// Callback to parse and handle a received wifi packet. Returns whether a socket took it.
    static bool OnProxyPacketReceived(const Network::ProxyPacket& packet);

    // Callback identifier for the OnProxyPacketReceived event. bsd:s, bsd:u and bsd:a share
    // file_descriptors, so they bind once; otherwise each packet is queued once per service.
    static inline Network::RoomMember::CallbackHandle<Network::ProxyPacket> proxy_packet_received;
    static inline std::mutex proxy_binding_mutex;
    static inline size_t proxy_binding_count = 0;

protected:
    std::unique_lock<std::mutex> LockService() noexcept override;
    bool is_user = false;
};

class BSDCFG final : public ServiceFramework<BSDCFG> {
public:
    explicit BSDCFG(Core::System& system_, const char *name);
    ~BSDCFG() override;
};

class BSD_NU final : public ServiceFramework<BSD_NU> {
public:
    explicit BSD_NU(Core::System& system_);
    ~BSD_NU() override;
};

} // namespace Service::Sockets
