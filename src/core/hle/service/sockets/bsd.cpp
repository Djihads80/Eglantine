// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/ranges.h>

#include "common/logging.h"
#include "common/polyfill_thread.h"
#include "common/socket_types.h"
#include "common/zstd_compression.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/sockets/bsd.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/network.h"
#include "core/internal_network/socket_proxy.h"
#include "core/internal_network/sockets.h"
#include "core/memory.h"
#include "network/network.h"
#include <common/settings.h>

namespace Service::Sockets {

namespace {

bool IsConnectionBased(Type type) {
    switch (type) {
    case Type::STREAM:
        return true;
    case Type::DGRAM:
        return false;
    default:
        UNIMPLEMENTED_MSG("Unimplemented type={}", type);
        return false;
    }
}

template <typename T>
T GetValue(std::span<const u8> buffer) {
    T t{};
    std::memcpy(&t, buffer.data(), (std::min)(sizeof(T), buffer.size()));
    return t;
}

template <typename T>
void PutValue(std::span<u8> buffer, const T& t) {
    std::memcpy(buffer.data(), &t, (std::min)(sizeof(T), buffer.size()));
}

/// Copies a socket address out as the BSD kernel does: its sa_len bytes, cut to the guest buffer.
/// The buffer shrinks to the bytes written, which the reply reports as the address length.
void PutSockAddr(std::vector<u8>& buffer, const SockAddrIn& addr) {
    buffer.resize((std::min)(buffer.size(), static_cast<size_t>(addr.len)));
    PutValue(buffer, addr);
}

/// Whether a guest fd_set holds descriptor fd. Its fd_mask words are little-endian, so fd is bit
/// fd % 8 of byte fd / 8 whatever the word size; a set too small to hold fd does not.
bool FdIsSet(std::span<const u8> set, size_t fd) {
    return fd / 8 < set.size() && ((set[fd / 8] >> (fd % 8)) & 1) != 0;
}

/// recvmmsg flag: receive without waiting after the first message.
constexpr u32 FLAG_MSG_WAITFORONE = 0x80000;
/// sendmmsg flag: raise no SIGPIPE, which the system never raises anyway.
constexpr u32 FLAG_MSG_NOSIGNAL = 0x20000;

using SelectClock = std::chrono::steady_clock;

/// When a wait bounded by a socket timeout in milliseconds ends; never for 0, no timeout.
SelectClock::time_point TimeoutDeadline(u32 timeout_ms) {
    if (timeout_ms == 0) {
        return SelectClock::time_point::max();
    }
    return SelectClock::now() + std::chrono::milliseconds{timeout_ms};
}

/// Opens a non-blocking host loopback UDP socket connected to its own address, so that it receives
/// what it sends: one datagram on it wakes a host poll.
Network::Errno OpenSignalSocket(Network::Socket& socket) {
    Network::Errno host_errno =
        socket.Initialize(Network::Domain::INET, Network::Type::DGRAM, Network::Protocol::UDP);
    if (host_errno != Network::Errno::SUCCESS) {
        return host_errno;
    }
    host_errno = socket.Bind(Network::SockAddrIn{
        .family = Network::Domain::INET,
        .ip = {127, 0, 0, 1},
        .portno = 0,
    });
    if (host_errno != Network::Errno::SUCCESS) {
        return host_errno;
    }
    const auto [address, name_errno] = socket.GetSockName();
    if (name_errno != Network::Errno::SUCCESS) {
        return name_errno;
    }
    host_errno = socket.Connect(address);
    if (host_errno != Network::Errno::SUCCESS) {
        return host_errno;
    }
    return socket.SetNonBlock(true);
}

/// The serialized messages of RecvMMsg or SendMMsg, from their in/out buffer; none if unreadable.
std::vector<u8> ReadMessages(HLERequestContext& ctx) {
    std::vector<u8> messages;
    if (!ctx.BufferDescriptorB().empty()) {
        const auto& buffer = ctx.BufferDescriptorB()[0];
        messages.resize(buffer.Size());
        if (!ctx.GetMemory().ReadBlock(buffer.Address(), messages.data(), messages.size())) {
            messages.clear();
        }
    }
    return messages;
}

/// The fields of a message serialized for RecvMMsg or SendMMsg, in its buffer.
struct SerializedMessage {
    std::span<u8> name;
    std::span<u8> control;
    std::span<u8> msg_flags;
    std::span<u8> msg_len;
};

/// Takes the fields of the messages serialized for RecvMMsg or SendMMsg, in order.
class MessageParser {
public:
    explicit MessageParser(std::span<u8> buffer_) : buffer{buffer_} {}

    /// The next size bytes; nullopt, taking nothing, if fewer are left.
    std::optional<std::span<u8>> Take(u64 size) {
        if (buffer.size() - offset < size) {
            return std::nullopt;
        }
        const std::span<u8> field = buffer.subspan(offset, static_cast<size_t>(size));
        offset += static_cast<size_t>(size);
        return field;
    }

    /// The next field after its size, a u64 for size_width 8 and a u32 otherwise.
    std::optional<std::span<u8>> TakeSized(size_t size_width) {
        const auto size_field = Take(size_width);
        if (!size_field) {
            return std::nullopt;
        }
        return Take(size_width == sizeof(u64) ? GetValue<u64>(*size_field)
                                              : GetValue<u32>(*size_field));
    }

    /// The next message, with its iovs put in iovs; nullopt if the buffer ends first. Its layout:
    /// a u32 name size and the name, a u32 iov count and each iov as a u64 size and its data, a
    /// u32 control size and the control data, then u32 msg_flags and u32 msg_len.
    std::optional<SerializedMessage> TakeMessage(std::vector<std::span<u8>>& iovs) {
        iovs.clear();
        const auto name = TakeSized(sizeof(u32));
        const auto iov_count = name ? Take(sizeof(u32)) : std::nullopt;
        if (!iov_count) {
            return std::nullopt;
        }
        // Each iov takes at least its size field, so a count past what the buffer holds fails
        // rather than growing iovs past the buffer's size.
        const u32 count = GetValue<u32>(*iov_count);
        for (u32 index = 0; index < count; ++index) {
            const auto iov = TakeSized(sizeof(u64));
            if (!iov) {
                return std::nullopt;
            }
            iovs.push_back(*iov);
        }
        const auto control = TakeSized(sizeof(u32));
        const auto msg_flags = control ? Take(sizeof(u32)) : std::nullopt;
        const auto msg_len = msg_flags ? Take(sizeof(u32)) : std::nullopt;
        if (!msg_len) {
            return std::nullopt;
        }
        return SerializedMessage{
            .name = *name,
            .control = *control,
            .msg_flags = *msg_flags,
            .msg_len = *msg_len,
        };
    }

private:
    std::span<u8> buffer;
    size_t offset = 0;
};

} // Anonymous namespace

/// An eventfd: a counter that writes add to and reads take. Select and Poll look at the counter,
/// and at its version to tell when to look again; so do the reads and writes that have to wait,
/// which are deferred rather than block a thread of the service.
struct BSD_USA::EventFdState {
    /// nn::socket::EventFdFlags
    static constexpr u32 FLAG_SEMAPHORE = 1;
    static constexpr u32 FLAG_NONBLOCK = 4;
    static constexpr u64 MAX_COUNTER = 0xFFFF'FFFF'FFFF'FFFE;

    std::mutex mutex;
    u64 counter = 0;
    /// Bumped, with mutex held, whenever the counter changes, and when a descriptor of the eventfd
    /// closes, so that the waits taken on that descriptor end even should the same eventfd be put
    /// back in its slot. Read without mutex by the notifier, under file_descriptors_mutex.
    std::atomic<u64> version{0};
    /// The file status flags that Fcntl gets and sets, O_NONBLOCK among them. They belong to the
    /// eventfd rather than to a descriptor, so duplicates share them, as with FreeBSD's dup.
    std::atomic<s32> status_flags{0};
    bool semaphore = false;
    /// Set once the last descriptor of the eventfd closed.
    bool closed = false;

    /// Whether a read or write that has to wait fails with EAGAIN instead.
    bool IsNonBlocking() const {
        return (status_flags.load(std::memory_order_relaxed) & Network::FLAG_O_NONBLOCK) != 0;
    }

    /// Takes from the counter without waiting. At zero, fails with EAGAIN and sets blocked_version
    /// to the version of the counter found so, which a deferred read waits on.
    std::pair<s32, Errno> Read(std::span<u8> message, u64& blocked_version) {
        if (message.size() < sizeof(u64)) {
            return {-1, Errno::INVAL};
        }
        std::scoped_lock lock{mutex};
        if (closed) {
            return {-1, Errno::BADF};
        }
        if (counter == 0) {
            blocked_version = version.load(std::memory_order_relaxed);
            return {-1, Errno::AGAIN};
        }
        const u64 value = semaphore ? 1 : counter;
        counter -= value;
        version.fetch_add(1, std::memory_order_relaxed);
        std::memcpy(message.data(), &value, sizeof(value));
        return {static_cast<s32>(sizeof(value)), Errno::SUCCESS};
    }

    /// Adds to the counter without waiting. When the sum would pass MAX_COUNTER, fails with EAGAIN
    /// and sets blocked_version to the version of the counter found so, which a deferred write
    /// waits on.
    std::pair<s32, Errno> Write(std::span<const u8> message, u64& blocked_version) {
        if (message.size() < sizeof(u64)) {
            return {-1, Errno::INVAL};
        }
        u64 value;
        std::memcpy(&value, message.data(), sizeof(value));
        if (value == (std::numeric_limits<u64>::max)()) {
            return {-1, Errno::INVAL};
        }
        std::scoped_lock lock{mutex};
        if (closed) {
            return {-1, Errno::BADF};
        }
        if (value > MAX_COUNTER - counter) {
            blocked_version = version.load(std::memory_order_relaxed);
            return {-1, Errno::AGAIN};
        }
        counter += value;
        version.fetch_add(1, std::memory_order_relaxed);
        return {static_cast<s32>(sizeof(value)), Errno::SUCCESS};
    }

    /// The counter's version, and whether a read and a write of 1 would not block.
    std::tuple<u64, bool, bool> Readiness() {
        std::scoped_lock lock{mutex};
        return {version.load(std::memory_order_relaxed), counter != 0, counter < MAX_COUNTER};
    }

    /// Bumps the version as a descriptor of the eventfd closes. Called with file_descriptors_mutex
    /// held, so that the table and the version change together for the notifier.
    void DescriptorClosed() {
        std::scoped_lock lock{mutex};
        version.fetch_add(1, std::memory_order_relaxed);
    }

    /// Has reads and writes fail from now on, once the last descriptor closed.
    void Close() {
        std::scoped_lock lock{mutex};
        closed = true;
    }
};

struct BSD_USA::SelectRequest {
    /// For a Select: how many descriptors it examines, from 0 up, and its read, write and except
    /// sets as the guest sent them over those descriptors, rounded up to whole 64-bit fd_mask
    /// words; an empty set for a null one.
    size_t nfds = 0;
    std::array<std::vector<u8>, 3> fd_sets;
    /// For a Poll: its pollfds as the guest sent them.
    std::vector<PollFD> pollfds;
    /// SelectClock::time_point::max() for no timeout.
    SelectClock::time_point deadline = SelectClock::time_point::max();
    /// For an eventfd Read or Write: the generation of the descriptor's slot. The retry goes on
    /// only if the descriptor has not closed since.
    u64 file_descriptor_generation = 0;
    /// For a RecvMMsg or SendMMsg: how many of its messages are done. Its buffer in the guest is
    /// only written by the reply, so the retry parses the messages from it again.
    u32 messages_done = 0;
    /// For a SendMMsg: how much of the next message a blocking stream socket has sent.
    size_t message_offset = 0;
    /// For a RecvMMsg: the reply serialized so far, with the messages done.
    std::vector<u8> reply;
};

struct BSD_USA::SelectWait {
    struct Entry {
        s32 fd;
        /// The descriptor as found open, to tell it from a later one at fd. The references keep
        /// what the notifier polls alive.
        std::shared_ptr<Network::SocketBase> socket;
        std::shared_ptr<EventFdState> event_fd;
        /// For an eventfd, the version of its counter as found.
        u64 event_fd_version = 0;
        /// For an eventfd Read or Write, the generation of the descriptor's slot.
        std::optional<u64> generation;
        /// For a socket, the events it waits for; none leaves it out of the wait.
        Network::PollEvents events{};
        /// Whether the socket has no host socket, so that EmulatedPoll examines it.
        bool emulated = false;
    };

    std::vector<Entry> entries;
    /// When to retry regardless, to look again at the sockets left out of the wait;
    /// SelectClock::time_point::max() for never.
    SelectClock::time_point recheck = SelectClock::time_point::max();
};

/// Its thread host-polls the sockets that deferred requests wait on, and signals the deferral
/// event, which retries every deferred request, once one of them may be done: a host socket
/// reports an event, an emulated socket is ready, an eventfd's counter or a descriptor changed, a
/// deadline or recheck time passed, or socket operations were cancelled. A retry examines its
/// descriptors again from the table and defers again if it has to.
///
/// The thread validates each wait against the table before it polls, under the table lock, and
/// keeps references to the sockets it polls until the poll ends. A descriptor closing its host
/// socket has the poll end first (see NotifyChange), so no host socket closes under it.
///
/// Lock order: mutex, then file_descriptors_mutex, then an eventfd's or a room socket's own.
class BSD_USA::SelectNotifier {
public:
    SelectNotifier(Core::System& system, Kernel::KEvent* deferral_event_)
        : kernel{system.Kernel()}, deferral_event{deferral_event_} {
        if (const Network::Errno host_errno = OpenSignalSocket(wake_socket);
            host_errno != Network::Errno::SUCCESS) {
            LOG_ERROR(Service, "Failed to open the wake socket of Select, which cannot wait: {}",
                      static_cast<int>(host_errno));
            stopped = true;
            return;
        }
        thread = kernel.RunOnHostCoreThread("bsdsocket:select", [this] { Run(); });
    }

    ~SelectNotifier() {
        {
            std::scoped_lock lock{mutex};
            stopped = true;
            Wake();
        }
        if (thread.joinable()) {
            thread.join();
        }
        // The notifier holds the writable side of the event; the server manager closes the
        // readable side.
        deferral_event->Close(kernel);
    }

    /// Takes the request of ctx if it is being retried, which it no longer waits for; nullopt for
    /// a request sent anew. Every handler that can defer calls this first, so that a run that
    /// completes leaves nothing behind for a later request at the same address.
    std::optional<SelectRequest> Take(const HLERequestContext& ctx) {
        std::scoped_lock lock{mutex};
        const auto it = requests.find(&ctx);
        if (it == requests.end()) {
            return std::nullopt;
        }
        SelectRequest request = std::move(it->second.request);
        requests.erase(it);
        return request;
    }

    /// Defers ctx until it may be done, moving its request and wait in. Returns false, leaving
    /// them, once requests can no longer wait, as emulation stops.
    bool Defer(const HLERequestContext& ctx, SelectRequest& request, SelectWait& wait) {
        std::scoped_lock lock{mutex};
        if (stopped) {
            return false;
        }
        requests.insert_or_assign(&ctx, Deferred{
                                            .request = std::move(request),
                                            .wait = std::move(wait),
                                            .id = ++last_id,
                                            .armed = true,
                                        });
        Wake();
        return true;
    }

    /// Forgets the deferred request of a context the server manager retires, as its client closed
    /// the session.
    void Cancel(const HLERequestContext& ctx) {
        std::scoped_lock lock{mutex};
        const auto it = requests.find(&ctx);
        if (it == requests.end()) {
            return;
        }
        const bool armed = it->second.armed;
        requests.erase(it);
        // The host poll lets go of its sockets.
        if (armed) {
            Wake();
        }
    }

    /// Has the deferred requests examined again, after an eventfd's counter, a room socket's
    /// queue or a descriptor changed. Given a socket whose host socket is about to close, also
    /// returns only once the host poll no longer has it; the caller has already taken it out of
    /// the table, so the next poll cannot.
    void NotifyChange(const Network::SocketBase* closing = nullptr) {
        std::unique_lock lock{mutex};
        // Out of a poll, the thread validates every wait before the next.
        if (!polling) {
            return;
        }
        const bool polls_closing =
            closing != nullptr &&
            std::any_of(polled_sockets.begin(), polled_sockets.end(),
                        [closing](const auto& socket) { return socket.get() == closing; });
        if (!polls_closing && std::none_of(requests.begin(), requests.end(),
                                           [](const auto& item) { return item.second.armed; })) {
            return;
        }
        const u64 epoch = poll_epoch;
        Wake();
        // Woken again in case the wake could not be sent.
        while (polls_closing && poll_epoch == epoch) {
            poll_done.wait_for(lock, std::chrono::milliseconds{50});
            Wake();
        }
    }

private:
    struct Deferred {
        SelectRequest request;
        SelectWait wait;
        /// Tells this wait from a later one of the same request.
        u64 id;
        /// Whether the thread watches the wait still: no longer once it signals for a retry.
        bool armed;
    };

    /// Ends the host poll in progress, if any. The caller holds mutex.
    void Wake() {
        if (!polling || wake_pending) {
            return;
        }
        constexpr std::array<u8, 1> token{};
        const Network::Errno host_errno = wake_socket.Send(token, 0).second;
        // A full receive buffer wakes the poll as well.
        wake_pending = host_errno == Network::Errno::SUCCESS || host_errno == Network::Errno::AGAIN;
        if (!wake_pending) {
            LOG_ERROR(Service, "Failed to wake Select's host poll: {}", static_cast<int>(host_errno));
        }
    }

    /// Whether the descriptors of a wait are as found: still open, with the same socket, eventfd,
    /// slot generation and eventfd counter version. The caller holds file_descriptors_mutex.
    static bool IsCurrent(const SelectWait& wait) {
        return std::all_of(
            wait.entries.begin(), wait.entries.end(), [](const SelectWait::Entry& entry) {
                const FileDescriptor* const descriptor = FindOpenDescriptor(entry.fd);
                return descriptor != nullptr && descriptor->socket == entry.socket &&
                       descriptor->event_fd == entry.event_fd &&
                       (!entry.generation ||
                        file_descriptor_generations[entry.fd] == *entry.generation) &&
                       (!entry.event_fd || entry.event_fd->version.load(std::memory_order_relaxed) ==
                                               entry.event_fd_version);
            });
    }

    /// Whether an emulated socket of a wait is ready for what it waits for. The caller holds
    /// file_descriptors_mutex.
    static bool HasEmulatedReady(const SelectWait& wait) {
        return std::any_of(
            wait.entries.begin(), wait.entries.end(), [](const SelectWait::Entry& entry) {
                return entry.emulated && True(entry.events) &&
                       True(entry.socket->EmulatedPoll(entry.events).value_or(
                           Network::PollEvents{}));
            });
    }

    void Run() {
        // How long to wait after a failed host poll before the next.
        constexpr auto failure_backoff = std::chrono::milliseconds{50};

        std::vector<Network::PollFD> pollfds;
        // Per polled socket, the request and wait it is polled for.
        std::vector<std::pair<const HLERequestContext*, u64>> polled_for;
        std::unique_lock lock{mutex};
        while (true) {
            // Cancelled socket operations mean emulation stops: every deferred request is retried,
            // to fail with EINTR, and none waits any more.
            if (Network::SocketOperationsCancelled()) {
                stopped = true;
            }
            const auto now = SelectClock::now();
            auto wake_at = SelectClock::time_point::max();
            bool retry = false;
            pollfds.clear();
            polled_for.clear();
            polled_sockets.clear();
            {
                std::scoped_lock table_lock{file_descriptors_mutex};
                for (auto& [ctx, deferred] : requests) {
                    if (!deferred.armed) {
                        continue;
                    }
                    if (stopped || now >= deferred.request.deadline ||
                        now >= deferred.wait.recheck || !IsCurrent(deferred.wait) ||
                        HasEmulatedReady(deferred.wait)) {
                        deferred.armed = false;
                        retry = true;
                        continue;
                    }
                    wake_at = (std::min)({wake_at, deferred.request.deadline, deferred.wait.recheck});
                    for (const SelectWait::Entry& entry : deferred.wait.entries) {
                        if (entry.emulated || False(entry.events)) {
                            continue;
                        }
                        pollfds.push_back(Network::PollFD{
                            .socket = entry.socket.get(),
                            .events = entry.events,
                            .revents = Network::PollEvents{},
                        });
                        polled_for.emplace_back(ctx, deferred.id);
                        polled_sockets.push_back(entry.socket);
                    }
                }
            }
            if (retry) {
                polled_sockets.clear();
                lock.unlock();
                deferral_event->Signal(kernel);
                lock.lock();
                continue;
            }
            if (stopped) {
                return;
            }

            pollfds.push_back(Network::PollFD{
                .socket = &wake_socket,
                .events = Network::PollEvents::In,
                .revents = Network::PollEvents{},
            });
            s32 timeout = -1;
            if (wake_at != SelectClock::time_point::max()) {
                const auto remaining =
                    std::chrono::ceil<std::chrono::milliseconds>(wake_at - now).count();
                timeout = static_cast<s32>(std::clamp<std::chrono::milliseconds::rep>(
                    remaining, 0, (std::numeric_limits<s32>::max)()));
            }
            polling = true;
            lock.unlock();
            // The host poll also waits on the interrupt socket, so it ends when socket operations
            // are cancelled.
            const auto [polled, poll_errno] = Network::Poll(pollfds, timeout);
            lock.lock();
            polling = false;
            ++poll_epoch;
            polled_sockets.clear();
            poll_done.notify_all();

            const Network::PollEvents wake_revents = pollfds.back().revents;
            if (True(wake_revents & Network::PollEvents::In)) {
                std::array<u8, 16> sink;
                while (wake_socket.Recv(0, sink).first > 0) {
                }
            }
            wake_pending = false;

            // A failed poll retries every request it was for, which then polls by itself; the
            // next poll waits a while, should it fail at once again.
            const bool failed =
                polled < 0 || True(wake_revents & (Network::PollEvents::Err |
                                                   Network::PollEvents::Hup |
                                                   Network::PollEvents::Nval));
            if (failed) {
                LOG_ERROR(Service, "Select's host poll failed: {}", static_cast<int>(poll_errno));
            }
            for (size_t i = 0; i < polled_for.size(); ++i) {
                if (!failed && False(pollfds[i].revents)) {
                    continue;
                }
                const auto it = requests.find(polled_for[i].first);
                if (it != requests.end() && it->second.armed &&
                    it->second.id == polled_for[i].second) {
                    it->second.armed = false;
                    retry = true;
                }
            }
            if (retry || failed) {
                lock.unlock();
                if (retry) {
                    deferral_event->Signal(kernel);
                }
                if (failed) {
                    std::this_thread::sleep_for(failure_backoff);
                }
                lock.lock();
            }
        }
    }

    Kernel::KernelCore& kernel;
    Kernel::KEvent* const deferral_event;
    /// A loopback socket in every host poll, which a datagram to itself wakes.
    Network::Socket wake_socket;

    std::mutex mutex;
    /// Notified when a host poll ends.
    std::condition_variable poll_done;
    /// Keyed by the address of the deferred request's context, which the server manager keeps
    /// until the request completes or is retired; never dereferenced.
    std::unordered_map<const HLERequestContext*, Deferred> requests;
    u64 last_id = 0;
    /// The sockets of the host poll in progress, kept alive until it ends.
    std::vector<std::shared_ptr<Network::SocketBase>> polled_sockets;
    /// Counts the host polls ended.
    u64 poll_epoch = 0;
    bool polling = false;
    bool wake_pending = false;
    /// Set once requests can no longer wait.
    bool stopped = false;

    std::jthread thread;
};

std::shared_ptr<BSD_USA::SelectNotifier> BSD_USA::CreateSelectNotifier(
    Core::System& system, ServerManager& server_manager) {
    Kernel::KEvent* deferral_event{};
    server_manager.ManageDeferral(&deferral_event);
    auto notifier = std::make_shared<SelectNotifier>(system, deferral_event);
    // The services own the notifier; the server manager only tells it of the requests it retires.
    server_manager.SetDeferralCancelHandler(
        [weak_notifier = std::weak_ptr{notifier}](const HLERequestContext& ctx) {
            if (const auto locked_notifier = weak_notifier.lock()) {
                locked_notifier->Cancel(ctx);
            }
        });
    return notifier;
}

bool BSD_USA::DeferSocketWait(HLERequestContext& ctx, s32 fd, const FileDescriptor& descriptor,
                              u64 generation, bool send, SelectRequest& request) {
    if (Network::SocketOperationsCancelled()) {
        return false;
    }
    // RdNorm rather than In: on Windows, In also reports out-of-band data, which a receive does
    // not take, so the wait would end over and over.
    const Network::PollEvents events = send ? Network::PollEvents::Out : Network::PollEvents::RdNorm;
    request.file_descriptor_generation = generation;
    SelectWait wait{
        .entries = {SelectWait::Entry{
            .fd = fd,
            .socket = descriptor.socket,
            .generation = generation,
            .events = events,
            .emulated = descriptor.socket->EmulatedPoll(events).has_value(),
        }},
    };
    if (!select_notifier->Defer(ctx, request, wait)) {
        return false;
    }
    ctx.SetIsDeferred();
    return true;
}

void BSD_USA::AcceptWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->AcceptImpl(fd, write_buffer);
}

void BSD_USA::AcceptWork::Response(HLERequestContext& ctx) {
    if (write_buffer.size() > 0) {
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD_USA::ConnectWork::Execute(BSD_USA* bsd) {
    bsd_errno = bsd->ConnectImpl(fd, addr);
}

void BSD_USA::ConnectWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::RecvWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->RecvImpl(fd, flags, message);
}

void BSD_USA::RecvWork::Response(HLERequestContext& ctx) {
    ctx.WriteBuffer(message);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::RecvFromWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->RecvFromImpl(fd, flags, message, addr);
}

void BSD_USA::RecvFromWork::Response(HLERequestContext& ctx) {
    ctx.WriteBuffer(message, 0);
    if (!addr.empty()) {
        ctx.WriteBuffer(addr, 1);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(addr.size()));
}

void BSD_USA::SendWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->SendImpl(fd, flags, message);
}

void BSD_USA::SendWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::SendToWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->SendToImpl(fd, flags, message, addr);
}

void BSD_USA::SendToWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::RegisterClient(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 3};

    rb.Push(ResultSuccess);
    rb.Push<s32>(0); // bsd errno
}

void BSD_USA::StartMonitoring(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};

    rb.Push(ResultSuccess);
}

void BSD_USA::Socket(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 domain = rp.Pop<u32>();
    const u32 type = rp.Pop<u32>();
    const u32 protocol = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. domain={} type={} protocol={}", domain, type, protocol);

    const auto [fd, bsd_errno] = SocketImpl(Domain(domain), Type(type), Protocol(protocol));

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(fd);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::SocketExempt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 domain = rp.Pop<u32>();
    const u32 type = rp.Pop<u32>();
    const u32 protocol = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. domain={} type={} protocol={}", domain, type, protocol);

    auto [fd, bsd_errno] = SocketImpl(Domain(domain), Type(type), Protocol(protocol));
    if (bsd_errno == Errno::SUCCESS) {
        bsd_errno = ShutdownImpl(fd, 0);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(fd);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::Select(HLERequestContext& ctx) {
    // nfds, then at its 8-byte alignment the timeout: a timeval, and whether the guest passed none.
    struct InputParameters {
        s32 nfds;
        u32 reserved;
        s64 timeout_seconds;
        s64 timeout_microseconds;
        u8 timeout_is_null;
    };
    static_assert(sizeof(InputParameters) == 0x20);

    const auto reply = [&ctx](s32 ret, Errno bsd_errno) {
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(ret);
        rb.PushEnum(bsd_errno);
    };

    // A select that has to wait is deferred rather than answered, and this runs again whenever it
    // may be done, going on with the request as first sent: its sets and its deadline.
    std::optional<SelectRequest> request = select_notifier->Take(ctx);
    if (!request) {
        IPC::RequestParser rp{ctx};
        const auto input = rp.PopRaw<InputParameters>();
        const bool timeout_is_null = input.timeout_is_null != 0;

        LOG_DEBUG(Service, "called. nfds={} timeout={}s+{}us null={}", input.nfds,
                  input.timeout_seconds, input.timeout_microseconds, timeout_is_null);

        if (input.nfds < 0) {
            reply(-1, Errno::INVAL);
            return;
        }
        request.emplace();
        // As in FreeBSD, no descriptor past the last one that can be open is examined, and the
        // sets are copied in and out by whole 64-bit fd_mask words. The read, write and except
        // sets come in input buffers 0 to 2 and go back in output buffers 0 to 2; an empty buffer
        // stands for a null set. They are copied, as each output buffer is the same guest memory
        // as its input.
        request->nfds = (std::min)(static_cast<size_t>(input.nfds), MAX_FD);
        const size_t set_size = (request->nfds + 63) / 64 * 8;
        bool bad_descriptor = false;
        for (size_t set = 0; set < request->fd_sets.size(); ++set) {
            if (!ctx.CanReadBuffer(set)) {
                continue;
            }
            const std::span<const u8> buffer = ctx.ReadBuffer(set);
            const std::span<const u8> copied = buffer.first((std::min)(set_size, buffer.size()));
            request->fd_sets[set].assign(copied.begin(), copied.end());
            // No descriptor past the table can be open, so asking for one fails the select.
            const size_t end = (std::min)(static_cast<size_t>(input.nfds), buffer.size() * 8);
            for (size_t fd = MAX_FD; fd < end && !bad_descriptor; ++fd) {
                bad_descriptor = FdIsSet(buffer, fd);
            }
        }
        if (bad_descriptor) {
            reply(-1, Errno::BADF);
            return;
        }
        if (!timeout_is_null) {
            if (input.timeout_seconds < 0 || input.timeout_microseconds < 0 ||
                input.timeout_microseconds >= 1'000'000) {
                reply(-1, Errno::INVAL);
                return;
            }
            // As in FreeBSD, a timeout past INT32_MAX seconds waits without limit.
            if (input.timeout_seconds <= (std::numeric_limits<s32>::max)()) {
                request->deadline = SelectClock::now() +
                                    std::chrono::seconds{input.timeout_seconds} +
                                    std::chrono::microseconds{input.timeout_microseconds};
            }
        }
    }

    std::array<std::vector<u8>, 3> ready_sets;
    SelectWait wait;
    std::optional<std::pair<s32, Errno>> result = SelectImpl(*request, ready_sets, wait);
    if (!result) {
        if (select_notifier->Defer(ctx, *request, wait)) {
            // No reply is built, so the command buffer keeps the request for the retry.
            ctx.SetIsDeferred();
            return;
        }
        // Requests can no longer wait, as emulation stops.
        result.emplace(-1, Errno::INTR);
    }
    const auto [ret, bsd_errno] = *result;

    // As in FreeBSD, the sets go back only when select succeeds, if with none ready.
    if (ret >= 0) {
        for (size_t set = 0; set < ready_sets.size(); ++set) {
            const size_t size =
                ctx.CanWriteBuffer(set)
                    ? (std::min)(ready_sets[set].size(), ctx.GetWriteBufferSize(set))
                    : 0;
            if (size > 0) {
                ctx.WriteBuffer(ready_sets[set].data(), size, set);
            }
        }
    }
    reply(ret, bsd_errno);
}

void BSD_USA::Poll(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 nfds = rp.Pop<s32>();
    const s32 timeout = rp.Pop<s32>();

    std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
    const auto result = PollImpl(ctx, write_buffer, nfds, timeout);
    // A deferred poll is answered once retried; no reply is built, so the command buffer keeps
    // the request for the retry.
    if (!result) {
        return;
    }
    const auto [ret, bsd_errno] = *result;
    // As in FreeBSD, the pollfds go back only when the poll succeeds.
    if (ret >= 0) {
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::Accept(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    ExecuteWork(ctx, AcceptWork{
                         .fd = fd,
                         .write_buffer = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD_USA::Bind(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} addrlen={}", fd, ctx.GetReadBufferSize());
    BuildErrnoResponse(ctx, BindImpl(fd, ctx.ReadBuffer()));
}

void BSD_USA::Connect(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} addrlen={}", fd, ctx.GetReadBufferSize());

    ExecuteWork(ctx, ConnectWork{
                         .fd = fd,
                         .addr = ctx.ReadBuffer(),
                     });
}

void BSD_USA::GetPeerName(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
    const Errno bsd_errno = GetPeerNameImpl(fd, write_buffer);
    // As in FreeBSD, the address goes back only on success, cut to its length, which the reply
    // reports; a failure leaves the guest's buffer and length as they were.
    if (bsd_errno == Errno::SUCCESS && !write_buffer.empty()) {
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno != Errno::SUCCESS ? -1 : 0);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD_USA::GetSockName(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
    const Errno bsd_errno = GetSockNameImpl(fd, write_buffer);
    // As in FreeBSD, the address goes back only on success, cut to its length, which the reply
    // reports; a failure leaves the guest's buffer and length as they were.
    if (bsd_errno == Errno::SUCCESS && !write_buffer.empty()) {
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno != Errno::SUCCESS ? -1 : 0);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD_USA::GetSockOpt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 level = rp.Pop<u32>();
    const auto optname = static_cast<OptName>(rp.Pop<u32>());

    std::vector<u8> optval(ctx.GetWriteBufferSize());

    LOG_DEBUG(Service, "called. fd={} level={} optname={:#x} len={:#x}", fd, level, optname,
              optval.size());

    const Errno err = GetSockOptImpl(fd, level, optname, optval);

    // As in FreeBSD, the value goes back only on success, cut to its length, which the reply
    // reports; a failure leaves the guest's buffer and length as they were.
    if (err == Errno::SUCCESS && !optval.empty()) {
        ctx.WriteBuffer(optval);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(err == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(err);
    rb.Push<u32>(static_cast<u32>(optval.size()));
}

void BSD_USA::Listen(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 backlog = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} backlog={}", fd, backlog);

    BuildErrnoResponse(ctx, ListenImpl(fd, backlog));
}

void BSD_USA::Fcntl(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 cmd = rp.Pop<s32>();
    const s32 arg = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} cmd={} arg={}", fd, cmd, arg);

    const auto [ret, bsd_errno] = FcntlImpl(fd, static_cast<FcntlCmd>(cmd), arg);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::SetSockOpt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 level = rp.Pop<u32>();
    const OptName optname = static_cast<OptName>(rp.Pop<u32>());
    const auto optval = ctx.ReadBuffer();

    LOG_DEBUG(Service, "called. fd={} level={} optname={:#x} optlen={}", fd, level,
              static_cast<u32>(optname), optval.size());

    BuildErrnoResponse(ctx, SetSockOptImpl(fd, level, optname, optval));
}

void BSD_USA::Shutdown(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const s32 how = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} how={}", fd, how);

    BuildErrnoResponse(ctx, ShutdownImpl(fd, how));
}

void BSD_USA::Recv(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags={:#x} len={}", fd, flags, ctx.GetWriteBufferSize());

    ExecuteWork(ctx, RecvWork{
                         .fd = fd,
                         .flags = flags,
                         .message = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD_USA::RecvFrom(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags={:#x} len={} addrlen={}", fd, flags,
              ctx.GetWriteBufferSize(0), ctx.GetWriteBufferSize(1));

    ExecuteWork(ctx, RecvFromWork{
                         .fd = fd,
                         .flags = flags,
                         .message = std::vector<u8>(ctx.GetWriteBufferSize(0)),
                         .addr = std::vector<u8>(ctx.GetWriteBufferSize(1)),
                     });
}

void BSD_USA::Send(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags={:#x} len={}", fd, flags, ctx.GetReadBufferSize());

    ExecuteWork(ctx, SendWork{
                         .fd = fd,
                         .flags = flags,
                         .message = ctx.ReadBuffer(),
                     });
}

void BSD_USA::SendTo(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags=0x{} len={} addrlen={}", fd, flags,
              ctx.GetReadBufferSize(0), ctx.GetReadBufferSize(1));

    ExecuteWork(ctx, SendToWork{
                         .fd = fd,
                         .flags = flags,
                         .message = ctx.ReadBuffer(0),
                         .addr = ctx.ReadBuffer(1),
                     });
}

void BSD_USA::RecvMMsg(HLERequestContext& ctx) {
    // The timeout is a timespec at its 8-byte alignment.
    struct InputParameters {
        s32 fd;
        u32 vlen;
        u32 flags;
        u32 reserved;
        s64 timeout_seconds;
        s64 timeout_nanoseconds;
    };
    static_assert(sizeof(InputParameters) == 0x20);

    IPC::RequestParser rp{ctx};
    const auto input = rp.PopRaw<InputParameters>();

    // The messages arrive serialized in the in/out buffer, which takes the received ones back.
    std::vector<u8> messages = ReadMessages(ctx);

    LOG_DEBUG(Service, "called. fd={} vlen={} flags={:#x} timeout={}s+{}ns len={}", input.fd,
              input.vlen, input.flags, input.timeout_seconds, input.timeout_nanoseconds,
              messages.size());

    const auto result = RecvMMsgImpl(ctx, input.fd, input.vlen, input.flags,
                                     input.timeout_seconds, input.timeout_nanoseconds, messages);
    // A deferred call is answered once retried; no reply is built, so the command buffer keeps
    // the request for the retry.
    if (!result) {
        return;
    }
    const auto [ret, bsd_errno] = *result;
    // The reply carries the received messages.
    if (ret > 0) {
        ctx.WriteBuffer(messages);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::SendMMsg(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 vlen = rp.Pop<u32>();
    const u32 flags = rp.Pop<u32>();

    // The messages arrive serialized in the in/out buffer, which takes their msg_len back.
    std::vector<u8> messages = ReadMessages(ctx);

    LOG_DEBUG(Service, "called. fd={} vlen={} flags={:#x} len={}", fd, vlen, flags,
              messages.size());

    const auto result = SendMMsgImpl(ctx, fd, vlen, flags, messages);
    // A deferred call is answered once retried; no reply is built, so the command buffer keeps
    // the request for the retry.
    if (!result) {
        return;
    }
    const auto [ret, bsd_errno] = *result;
    // msg_len of each sent message was filled in.
    if (ret > 0) {
        ctx.WriteBuffer(messages);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::Write(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} len={}", fd, ctx.GetReadBufferSize());

    const auto result = WriteImpl(ctx, fd, ctx.ReadBuffer());
    // A deferred write is answered once retried; no reply is built, so the command buffer keeps
    // the request for the retry.
    if (!result) {
        return;
    }
    const auto [ret, bsd_errno] = *result;

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::Read(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} len={}", fd, ctx.GetWriteBufferSize());

    std::vector<u8> message(ctx.GetWriteBufferSize());
    const auto result = ReadImpl(ctx, fd, message);
    // A deferred read is answered once retried; no reply is built, so the command buffer keeps
    // the request for the retry.
    if (!result) {
        return;
    }
    const auto [ret, bsd_errno] = *result;
    if (ret > 0) {
        ctx.WriteBuffer(message.data(), static_cast<size_t>(ret));
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::Close(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    BuildErrnoResponse(ctx, CloseImpl(fd));
}

/// @brief Only bsd:s is able to dup()
void BSD_USA::DuplicateSocket(HLERequestContext& ctx) {
    struct InputParameters {
        s32 fd;
        u64 reserved;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    struct OutputParameters {
        s32 ret;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0x8);

    IPC::RequestParser rp{ctx};
    auto input = rp.PopRaw<InputParameters>();

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    if (is_user) {
        rb.PushRaw(OutputParameters{
            .ret = 0,
            .bsd_errno = Errno::INVAL,
        });
        return;
    }

    auto const res_v = DuplicateSocketImpl(input.fd);
    if (auto* res = std::get_if<s32>(&res_v)) {
        rb.PushRaw(OutputParameters{
            .ret = *res,
            .bsd_errno = Errno::SUCCESS,
        });
    } else {
        auto* err = std::get_if<Errno>(&res_v);
        rb.PushRaw(OutputParameters{
            .ret = 0,
            .bsd_errno = *err,
        });
    }
}

void BSD_USA::EventFd(HLERequestContext& ctx) {
    // The flags come first in the request; initval follows at its 8-byte alignment.
    struct InputParameters {
        u32 flags;
        u64 initval;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto input = rp.PopRaw<InputParameters>();

    LOG_DEBUG(Service, "called. initval={} flags={:#x}", input.initval, input.flags);

    const auto [fd, bsd_errno] = EventFdImpl(input.initval, input.flags);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(fd);
    rb.PushEnum(bsd_errno);
}

template <typename Work>
void BSD_USA::ExecuteWork(HLERequestContext& ctx, Work work) {
    work.Execute(this);
    work.Response(ctx);
}

std::pair<s32, Errno> BSD_USA::SocketImpl(Domain domain, Type type, Protocol protocol) {
    // This network backend supports IPv4 only. Let callers fall back instead of returning a
    // descriptor backed by a failed host socket.
    if (domain != Domain::Unspecified && domain != Domain::INET) {
        return {-1, Errno::AFNOSUPPORT};
    }

    // user bsd:u has restrictions on SOCK_SEQPACKET and SOCK_RAW
    if (is_user && (type == Type::SEQPACKET || type == Type::RAW)) {
        if (type == Type::RAW && domain == Domain::INET && protocol == Protocol::ICMP) {
            // fine, can use on bsd:s and bsd:u
        } else {
            return {-1, Errno::INVAL};
        }
    }

    [[maybe_unused]] const bool unk_flag = (static_cast<u32>(type) & 0x20000000) != 0;
    UNIMPLEMENTED_IF_MSG(unk_flag, "Unknown flag in type");
    type = static_cast<Type>(static_cast<u32>(type) & ~0x20000000);

    // Checked before taking an fd. A joined room does not lift it: room TCP is only stubbed.
    if (Settings::values.airplane_mode.GetValue() && IsConnectionBased(type)) {
        LOG_ERROR(Service, "Airplane mode is enabled, cannot create socket");
        return {-1, Errno::NOTCONN};
    }

    std::shared_ptr<Network::SocketBase> socket;
    auto room_member = Network::GetRoomMember().lock();
    if (room_member && room_member->IsConnected()) {
        socket = std::make_shared<Network::ProxySocket>();
    } else {
        socket = std::make_shared<Network::Socket>();
    }
    socket->Initialize(Translate(domain), Translate(type), Translate(protocol));

    // Publish the socket only once it is set up, since the room thread delivers packets to it
    std::scoped_lock lock{file_descriptors_mutex};
    const s32 fd = FindFreeFileDescriptorHandle();
    if (fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return {-1, Errno::MFILE};
    }

    file_descriptors[fd] = FileDescriptor{
        .socket = std::move(socket),
        .is_connection_based = IsConnectionBased(type),
    };

    LOG_INFO(Service, "New socket fd={}", fd);

    return {fd, Errno::SUCCESS};
}

std::optional<std::pair<s32, Errno>> BSD_USA::PollImpl(HLERequestContext& ctx,
                                                       std::vector<u8>& write_buffer, s32 nfds,
                                                       s32 timeout) {
    using Outcome = std::pair<s32, Errno>;
    // How long a socket the wait cannot watch goes before the poll looks at it again.
    constexpr auto recheck_interval = std::chrono::milliseconds{50};
    // What poll reports whatever the events asked for.
    constexpr PollEvents always_reported = PollEvents::Err | PollEvents::Hup | PollEvents::Nval;

    // A poll that has to wait is deferred rather than answered, and this runs again whenever it
    // may be done, going on with the pollfds and deadline it was first sent with.
    std::optional<SelectRequest> request = select_notifier->Take(ctx);
    if (!request) {
        LOG_DEBUG(Service, "called. nfds={} timeout={}", nfds, timeout);
        if (nfds <= 0) {
            // When no entries are provided, -1 is returned with errno zero
            return Outcome{-1, Errno::SUCCESS};
        }
        const size_t size = static_cast<size_t>(nfds) * sizeof(PollFD);
        const std::span<const u8> read_buffer = ctx.ReadBuffer();
        if (read_buffer.size() < size || write_buffer.size() < size) {
            return Outcome{-1, Errno::INVAL};
        }
        // A negative timeout other than -1, which waits without limit, is invalid.
        if (timeout < -1) {
            return Outcome{-1, Errno::INVAL};
        }
        request.emplace();
        request->pollfds.resize(static_cast<size_t>(nfds));
        std::memcpy(request->pollfds.data(), read_buffer.data(), size);
        if (timeout >= 0) {
            request->deadline = SelectClock::now() + std::chrono::milliseconds{timeout};
        }
    }
    std::vector<PollFD> fds = request->pollfds;

    SelectWait wait;
    // Whether a host socket is left out of the wait, to be looked at again a while later.
    bool recheck = false;
    // The host poll, and per polled socket the index of its pollfd and of its wait entry.
    std::vector<Network::PollFD> host_pollfds;
    std::vector<size_t> host_entries;
    std::vector<size_t> host_wait_entries;
    {
        // No descriptor can close while the table is locked, so neither can a host socket under
        // the poll, which does not wait.
        std::scoped_lock lock{file_descriptors_mutex};
        for (size_t i = 0; i < fds.size(); ++i) {
            PollFD& pollfd = fds[i];
            pollfd.revents = PollEvents{};
            // As in FreeBSD, a negative descriptor is left out, and one that is not open is
            // reported invalid.
            if (pollfd.fd < 0) {
                continue;
            }
            const FileDescriptor* const descriptor = FindOpenDescriptor(pollfd.fd);
            if (descriptor == nullptr) {
                LOG_TRACE(Service, "File descriptor handle={} is not open", pollfd.fd);
                pollfd.revents = PollEvents::Nval;
                continue;
            }
            const Network::PollEvents events = Translate(pollfd.events);
            SelectWait::Entry entry{
                .fd = pollfd.fd,
                .socket = descriptor->socket,
                .event_fd = descriptor->event_fd,
            };
            if (descriptor->event_fd) {
                // An eventfd is ready by its counter.
                const auto [version, readable, writable] = descriptor->event_fd->Readiness();
                entry.event_fd_version = version;
                Network::PollEvents revents{};
                if (readable) {
                    revents |= events & (Network::PollEvents::In | Network::PollEvents::RdNorm);
                }
                if (writable) {
                    revents |= events & Network::PollEvents::Out;
                }
                pollfd.revents = Translate(revents);
            } else if (const auto emulated = descriptor->socket->EmulatedPoll(events)) {
                entry.events = events;
                entry.emulated = true;
                pollfd.revents = Translate(*emulated);
            } else if (!descriptor->socket->IsOpened()) {
                LOG_TRACE(Service, "File descriptor handle={} has no host socket", pollfd.fd);
                pollfd.revents = PollEvents::Nval;
                continue;
            } else {
                // A hangup or an error, which the host poll reports whatever the events, cannot
                // wake a wait on no events; such a socket is looked at again a while later.
                entry.events = events;
                recheck |= False(events);
                host_entries.push_back(i);
                host_wait_entries.push_back(wait.entries.size());
                host_pollfds.push_back(Network::PollFD{
                    .socket = descriptor->socket.get(),
                    .events = events,
                    .revents = Network::PollEvents{},
                });
            }
            wait.entries.push_back(std::move(entry));
        }
        if (!host_pollfds.empty()) {
            const auto [polled, poll_errno] = Network::Poll(host_pollfds, 0);
            if (polled < 0) {
                return Outcome{-1, Network::SocketOperationsCancelled() ? Errno::INTR
                                                                         : Translate(poll_errno)};
            }
        }
    }
    for (size_t i = 0; i < host_pollfds.size(); ++i) {
        PollFD& pollfd = fds[host_entries[i]];
        const PollEvents reported = Translate(host_pollfds[i].revents);
        // As in FreeBSD, only the events asked for are reported, besides those always reported.
        pollfd.revents = reported & (pollfd.events | always_reported);
        // An event that the host poll keeps reporting but that is not to be reported would end
        // every wait at once. The socket is left out of the wait and looked at again a while
        // later instead.
        if (True(reported) && False(pollfd.revents)) {
            wait.entries[host_wait_entries[i]].events = Network::PollEvents{};
            recheck = true;
        }
    }

    // As in FreeBSD, the result counts the descriptors with any event to report.
    const s32 count = static_cast<s32>(std::count_if(
        fds.begin(), fds.end(), [](const PollFD& pollfd) { return True(pollfd.revents); }));
    const auto now = SelectClock::now();
    if (count == 0 && now < request->deadline) {
        // A cancellation of socket operations, as on emulation shutdown, ends the wait.
        if (Network::SocketOperationsCancelled()) {
            return Outcome{-1, Errno::INTR};
        }
        // Waiting here would hold one of the few threads of the service, which the request
        // that ends the wait may need. The poll is deferred instead.
        if (recheck) {
            wait.recheck = now + recheck_interval;
        }
        if (!select_notifier->Defer(ctx, *request, wait)) {
            // Requests can no longer wait, as emulation stops.
            return Outcome{-1, Errno::INTR};
        }
        ctx.SetIsDeferred();
        return std::nullopt;
    }

    write_buffer.resize(fds.size() * sizeof(PollFD));
    std::memcpy(write_buffer.data(), fds.data(), write_buffer.size());
    return Outcome{count, Errno::SUCCESS};
}

std::optional<std::pair<s32, Errno>> BSD_USA::SelectImpl(const SelectRequest& request,
                                                         std::array<std::vector<u8>, 3>& ready_sets,
                                                         SelectWait& wait) {
    using Outcome = std::pair<s32, Errno>;
    constexpr size_t read_set = 0;
    constexpr size_t write_set = 1;
    constexpr size_t except_set = 2;

    // Readiness as FreeBSD's select takes it from poll events: a hangup or an error makes a
    // descriptor readable and writable, an error exceptional too. Exceptional otherwise means
    // out-of-band data pending, which the host poll reports as Pri.
    constexpr Network::PollEvents read_events =
        Network::PollEvents::RdNorm | Network::PollEvents::Hup | Network::PollEvents::Err;
    constexpr Network::PollEvents write_events =
        Network::PollEvents::Out | Network::PollEvents::Hup | Network::PollEvents::Err;
    constexpr Network::PollEvents except_events =
        Network::PollEvents::Pri | Network::PollEvents::Err;
    // How long a socket left out of the wait goes before the select looks at it again.
    constexpr auto recheck_interval = std::chrono::milliseconds{50};

    // Per entry of the wait, the sets the guest asks for its descriptor in, and those it is ready
    // for.
    std::vector<std::array<bool, 3>> wanted;
    std::vector<std::array<bool, 3>> ready;
    // The host poll, and per polled socket the index of its entry.
    std::vector<Network::PollFD> host_pollfds;
    std::vector<size_t> host_entries;
    wait.entries.clear();
    {
        // No descriptor can close while the table is locked, so neither can a host socket under
        // the poll, which does not wait.
        std::scoped_lock lock{file_descriptors_mutex};
        for (size_t fd = 0; fd < request.nfds; ++fd) {
            std::array<bool, 3> asked{};
            for (size_t set = 0; set < asked.size(); ++set) {
                asked[set] = FdIsSet(request.fd_sets[set], fd);
            }
            if (asked == std::array<bool, 3>{}) {
                continue;
            }
            // As in FreeBSD, a descriptor in any set that is not open fails the select.
            const FileDescriptor* const descriptor = FindOpenDescriptor(static_cast<s32>(fd));
            if (descriptor == nullptr) {
                LOG_DEBUG(Service, "File descriptor handle={} is not open", fd);
                return Outcome{-1, Errno::BADF};
            }
            SelectWait::Entry entry{
                .fd = static_cast<s32>(fd),
                .socket = descriptor->socket,
                .event_fd = descriptor->event_fd,
            };
            std::array<bool, 3> found{};
            if (descriptor->event_fd) {
                // An eventfd is ready by its counter; it has no exceptional condition.
                const auto [version, readable, writable] = descriptor->event_fd->Readiness();
                entry.event_fd_version = version;
                found = {asked[read_set] && readable, asked[write_set] && writable, false};
            } else {
                if (asked[read_set]) {
                    entry.events |= Network::PollEvents::RdNorm;
                }
                if (asked[write_set]) {
                    entry.events |= Network::PollEvents::Out;
                }
                if (asked[except_set]) {
                    entry.events |= Network::PollEvents::Pri;
                }
                if (const auto emulated = descriptor->socket->EmulatedPoll(entry.events)) {
                    // A room socket queues its packets in the emulator.
                    entry.emulated = true;
                    found = {True(*emulated & Network::PollEvents::RdNorm),
                             True(*emulated & Network::PollEvents::Out),
                             True(*emulated & Network::PollEvents::Pri)};
                } else if (!descriptor->socket->IsOpened()) {
                    LOG_DEBUG(Service, "File descriptor handle={} has no host socket", fd);
                    return Outcome{-1, Errno::BADF};
                } else {
                    host_entries.push_back(wait.entries.size());
                    host_pollfds.push_back(Network::PollFD{
                        .socket = descriptor->socket.get(),
                        .events = entry.events,
                        .revents = Network::PollEvents{},
                    });
                }
            }
            wanted.push_back(asked);
            ready.push_back(found);
            wait.entries.push_back(std::move(entry));
        }
        if (!host_pollfds.empty()) {
            const auto [polled, poll_errno] = Network::Poll(host_pollfds, 0);
            if (polled < 0) {
                return Outcome{-1, Network::SocketOperationsCancelled() ? Errno::INTR
                                                                         : Translate(poll_errno)};
            }
        }
    }

    const auto now = SelectClock::now();
    wait.recheck = SelectClock::time_point::max();
    for (size_t i = 0; i < host_pollfds.size(); ++i) {
        const size_t index = host_entries[i];
        const Network::PollEvents revents = host_pollfds[i].revents;
        if (True(revents & Network::PollEvents::Nval)) {
            return Outcome{-1, Errno::BADF};
        }
        ready[index] = {wanted[index][read_set] && True(revents & read_events),
                        wanted[index][write_set] && True(revents & write_events),
                        wanted[index][except_set] && True(revents & except_events)};
        // An event that does not make the socket ready, as a hangup is to one asked for only its
        // exceptional condition, would end every wait at once, as the host poll keeps reporting
        // it. The socket is left out of the wait and looked at again a while later instead.
        if (True(revents) && ready[index] == std::array<bool, 3>{}) {
            wait.entries[index].events = Network::PollEvents{};
            wait.recheck = now + recheck_interval;
        }
    }

    s32 count = 0;
    for (const std::array<bool, 3>& found : ready) {
        count += static_cast<s32>(std::count(found.begin(), found.end(), true));
    }
    if (count == 0 && now < request.deadline) {
        // A cancellation of socket operations, as on emulation shutdown, ends the wait.
        if (Network::SocketOperationsCancelled()) {
            return Outcome{-1, Errno::INTR};
        }
        return std::nullopt;
    }

    // As in FreeBSD, each set goes back holding only the descriptors found ready for it, over the
    // words copied in. The result counts set bits, so a descriptor ready in two sets counts twice.
    for (size_t set = 0; set < ready_sets.size(); ++set) {
        ready_sets[set].assign(request.fd_sets[set].size(), 0);
    }
    for (size_t index = 0; index < wait.entries.size(); ++index) {
        const size_t byte = static_cast<size_t>(wait.entries[index].fd) / 8;
        const u8 bit = static_cast<u8>(1U << (wait.entries[index].fd % 8));
        for (size_t set = 0; set < ready_sets.size(); ++set) {
            if (ready[index][set]) {
                ready_sets[set][byte] = static_cast<u8>(ready_sets[set][byte] | bit);
            }
        }
    }
    return Outcome{count, Errno::SUCCESS};
}

std::pair<s32, Errno> BSD_USA::AcceptImpl(s32 fd, std::vector<u8>& write_buffer) {
    const auto descriptor = CopyDescriptor(fd);
    if (!descriptor) {
        return {-1, Errno::BADF};
    }

    auto [result, bsd_errno] = descriptor->socket->Accept();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return {-1, Translate(bsd_errno)};
    }

    // Accept may block, so the fd is picked afterwards, in the same critical section as the write
    s32 new_fd;
    {
        std::scoped_lock lock{file_descriptors_mutex};
        new_fd = FindFreeFileDescriptorHandle();
        if (new_fd < 0) {
            LOG_ERROR(Service, "No more file descriptors available");
            return {-1, Errno::MFILE};
        }
        file_descriptors[new_fd] = FileDescriptor{
            .socket = std::move(result.socket),
            .is_connection_based = descriptor->is_connection_based,
            // As in FreeBSD, an accepted socket takes the listening socket's timeouts.
            .receive_timeout = descriptor->receive_timeout,
            .send_timeout = descriptor->send_timeout,
        };
    }

    const SockAddrIn guest_addr_in = Translate(result.sockaddr_in);
    PutSockAddr(write_buffer, guest_addr_in);

    return {new_fd, Errno::SUCCESS};
}

Errno BSD_USA::BindImpl(s32 fd, std::span<const u8> addr) {
    const auto descriptor = CopyDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }
    ASSERT(addr.size() >= 16);

    auto addr_in = GetValue<SockAddrIn>(addr);

    return Translate(descriptor->socket->Bind(Translate(addr_in)));
}

Errno BSD_USA::ConnectImpl(s32 fd, std::span<const u8> addr) {
    const auto descriptor = CopyDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }

    ASSERT(addr.size() >= 16);

    auto addr_in = GetValue<SockAddrIn>(addr);

    const Errno result = Translate(descriptor->socket->Connect(Translate(addr_in)));

    if (result == Errno::ISCONN) {
        LOG_DEBUG(Service, "returned ISCONN - socket already connected");
        return Errno::SUCCESS;
    }

    return result;
}

Errno BSD_USA::GetPeerNameImpl(s32 fd, std::vector<u8>& write_buffer) {
    const auto descriptor = CopyDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }

    const auto [addr_in, bsd_errno] = descriptor->socket->GetPeerName();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return Translate(bsd_errno);
    }
    PutSockAddr(write_buffer, Translate(addr_in));
    return Errno::SUCCESS;
}

Errno BSD_USA::GetSockNameImpl(s32 fd, std::vector<u8>& write_buffer) {
    const auto descriptor = CopyDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }

    const auto [addr_in, bsd_errno] = descriptor->socket->GetSockName();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return Translate(bsd_errno);
    }
    PutSockAddr(write_buffer, Translate(addr_in));
    return Errno::SUCCESS;
}

Errno BSD_USA::ListenImpl(s32 fd, s32 backlog) {
    const auto descriptor = CopyDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }
    return Translate(descriptor->socket->Listen(backlog));
}

std::pair<s32, Errno> BSD_USA::FcntlImpl(s32 fd, FcntlCmd cmd, s32 arg) {
    const auto descriptor = CopyFileDescriptor(fd);
    if (!descriptor) {
        return {-1, Errno::BADF};
    }

    switch (cmd) {
    case FcntlCmd::GETFL:
        ASSERT(arg == 0);
        if (descriptor->event_fd) {
            return {descriptor->event_fd->status_flags.load(std::memory_order_relaxed),
                    Errno::SUCCESS};
        }
        return {descriptor->flags, Errno::SUCCESS};
    case FcntlCmd::SETFL: {
        // An eventfd's flags only decide whether its reads and writes wait. They belong to the
        // eventfd, so its duplicates share them, as with FreeBSD's dup.
        if (descriptor->event_fd) {
            descriptor->event_fd->status_flags.store(arg, std::memory_order_relaxed);
            return {0, Errno::SUCCESS};
        }
        const bool enable = (arg & Network::FLAG_O_NONBLOCK) != 0;
        // Set the mode and the flags together, so IsBlocking never sees one without the other.
        // Calls on the socket never switch its mode for themselves, so this one stays.
        std::scoped_lock lock{file_descriptors_mutex};
        const Errno bsd_errno = Translate(descriptor->socket->SetNonBlock(enable));
        if (bsd_errno != Errno::SUCCESS) {
            return {-1, bsd_errno};
        }
        if (auto& entry = file_descriptors[fd]; entry && entry->socket == descriptor->socket) {
            entry->flags = arg;
        }
        return {0, Errno::SUCCESS};
    }
    default:
        UNIMPLEMENTED_MSG("Unimplemented cmd={}", cmd);
        return {-1, Errno::SUCCESS};
    }
}

Errno BSD_USA::GetSockOptImpl(s32 fd, u32 level, OptName optname, std::vector<u8>& optval) {
    const auto descriptor = CopyDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }

    Network::SocketBase* const socket = descriptor->socket.get();

    // As FreeBSD's sooptcopyout, the value is cut to the guest's buffer, which then shrinks to
    // what was written: the length the reply reports.
    const auto put_value = [&optval](const auto& value) {
        optval.resize((std::min)(optval.size(), sizeof(value)));
        PutValue(optval, value);
    };
    // gRPC sets TCP_NODELAY and SO_REUSEADDR on each connection's socket and reads them back,
    // closing the socket unless the read reports the option as set.
    const auto put_flag = [&put_value](std::pair<bool, Network::Errno> result) {
        const auto [enabled, getsockopt_err] = result;
        if (getsockopt_err != Network::Errno::SUCCESS) {
            return Translate(getsockopt_err);
        }
        put_value(u32{enabled ? 1U : 0U});
        return Errno::SUCCESS;
    };

    if (level == static_cast<u32>(SocketLevel::TCP)) {
        switch (static_cast<TcpOptName>(optname)) {
        case TcpOptName::NODELAY:
            return put_flag(socket->GetNoDelay());
        default:
            UNIMPLEMENTED_MSG("Unimplemented TCP getsockopt optname={:#x}",
                              static_cast<u32>(optname));
            return Errno::SUCCESS;
        }
    }

    if (level != static_cast<u32>(SocketLevel::SOCKET)) {
        UNIMPLEMENTED_MSG("Unknown getsockopt level");
        return Errno::SUCCESS;
    }

    switch (optname) {
    case OptName::REUSEADDR:
        return put_flag(socket->GetReuseAddr());
    case OptName::ERROR_: {
        auto [pending_err, getsockopt_err] = socket->GetPendingError();
        if (getsockopt_err == Network::Errno::SUCCESS) {
            put_value(Translate(pending_err));
        }
        return Translate(getsockopt_err);
    }
    default:
        UNIMPLEMENTED_MSG("Unimplemented optname={}", optname);
        return Errno::SUCCESS;
    }
}

Errno BSD_USA::SetSockOptImpl(s32 fd, u32 level, OptName optname, std::span<const u8> optval) {
    const auto descriptor = CopyDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }

    Network::SocketBase* const socket = descriptor->socket.get();

    // As FreeBSD's sooptcopyin, a value shorter than the option's fails with EINVAL, and the start
    // of a longer one is taken.
    if (level == static_cast<u32>(SocketLevel::TCP) &&
        static_cast<TcpOptName>(optname) == TcpOptName::NODELAY) {
        if (optval.size() < sizeof(u32)) {
            return Errno::INVAL;
        }
        return Translate(socket->SetNoDelay(GetValue<u32>(optval) != 0));
    }

    if (level != static_cast<u32>(SocketLevel::SOCKET)) {
        LOG_WARNING(Service, "(STUBBED) setsockopt with level={}, optname={}", level, optname);
        return Errno::SUCCESS;
    }

    if (optname == OptName::LINGER) {
        if (optval.size() < sizeof(Linger)) {
            return Errno::INVAL;
        }
        auto linger = GetValue<Linger>(optval);
        ASSERT(linger.onoff == 0 || linger.onoff == 1);

        return Translate(socket->SetLinger(linger.onoff != 0, linger.linger));
    }

    if (optval.size() < sizeof(u32)) {
        return Errno::INVAL;
    }
    auto value = GetValue<u32>(optval);

    switch (optname) {
    case OptName::REUSEADDR:
        ASSERT(value == 0 || value == 1);
        return Translate(socket->SetReuseAddr(value != 0));
    case OptName::KEEPALIVE:
        ASSERT(value == 0 || value == 1);
        return Translate(socket->SetKeepAlive(value != 0));
    case OptName::BROADCAST:
        ASSERT(value == 0 || value == 1);
        return Translate(socket->SetBroadcast(value != 0));
    case OptName::SNDBUF:
        return Translate(socket->SetSndBuf(value));
    case OptName::RCVBUF:
        return Translate(socket->SetRcvBuf(value));
    case OptName::SNDTIMEO:
    case OptName::RCVTIMEO: {
        // A timeval of two 64-bit fields, as libnx's and FreeBSD's; as FreeBSD's sooptcopyin, a
        // shorter value fails with EINVAL, and as its sosetopt, an out-of-range one with EDOM.
        if (optval.size() < 2 * sizeof(s64)) {
            return Errno::INVAL;
        }
        const s64 seconds = GetValue<s64>(optval);
        const s64 microseconds = GetValue<s64>(optval.subspan(sizeof(s64)));
        if (seconds < 0 || microseconds < 0 || microseconds >= 1'000'000) {
            return Errno::DOM;
        }
        // In milliseconds, rounded up so that a nonzero timeout stays one, and capped.
        constexpr s64 max_timeout_ms = (std::numeric_limits<u32>::max)();
        const u32 timeout_ms = static_cast<u32>(
            seconds >= max_timeout_ms / 1000
                ? max_timeout_ms
                : (std::min)(max_timeout_ms, seconds * 1000 + (microseconds + 999) / 1000));
        const bool receive = optname == OptName::RCVTIMEO;
        const Errno result = Translate(receive ? socket->SetRcvTimeo(timeout_ms)
                                               : socket->SetSndTimeo(timeout_ms));
        if (result == Errno::SUCCESS) {
            // Deferred receives and sends wait as long. The option belongs to the socket, which
            // its duplicates share.
            std::scoped_lock lock{file_descriptors_mutex};
            for (std::optional<FileDescriptor>& entry : file_descriptors) {
                if (entry && entry->socket == descriptor->socket) {
                    (receive ? entry->receive_timeout : entry->send_timeout) = timeout_ms;
                }
            }
        }
        return result;
    }
    case OptName::NOSIGPIPE:
        LOG_WARNING(Service, "(STUBBED) setting NOSIGPIPE to {}", value);
        return Errno::SUCCESS;
    default:
        UNIMPLEMENTED_MSG("Unimplemented optname={}", optname);
        return Errno::SUCCESS;
    }
}

Errno BSD_USA::ShutdownImpl(s32 fd, s32 how) {
    const auto descriptor = CopyDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }
    const Network::ShutdownHow host_how = Translate(static_cast<ShutdownHow>(how));
    return Translate(descriptor->socket->Shutdown(host_how));
}

std::pair<s32, Errno> BSD_USA::RecvImpl(s32 fd, u32 flags, std::vector<u8>& message) {
    const auto descriptor = CopyDescriptor(fd);
    if (!descriptor) {
        return {-1, Errno::BADF};
    }

    // MSG_DONTWAIT keeps this receive alone from waiting; the socket's mode is left alone, as
    // other calls on it, on other threads, depend on it.
    return Translate(descriptor->socket->Recv(flags, message));
}

std::pair<s32, Errno> BSD_USA::RecvFromImpl(s32 fd, u32 flags, std::vector<u8>& message,
                                        std::vector<u8>& addr) {
    const auto descriptor = CopyDescriptor(fd);
    if (!descriptor) {
        return {-1, Errno::BADF};
    }

    Network::SockAddrIn addr_in{};
    Network::SockAddrIn* p_addr_in = nullptr;
    if (descriptor->is_connection_based) {
        // Connection based file descriptors (e.g. TCP) zero addr
        addr.clear();
    } else {
        p_addr_in = &addr_in;
    }

    // MSG_DONTWAIT keeps this receive alone from waiting; the socket's mode is left alone, as
    // other calls on it, on other threads, depend on it.
    const auto [ret, bsd_errno] =
        Translate(descriptor->socket->RecvFrom(flags, message, p_addr_in));

    if (p_addr_in) {
        if (ret < 0) {
            addr.clear();
        } else {
            PutSockAddr(addr, Translate(addr_in));
        }
    }

    return {ret, bsd_errno};
}

std::pair<s32, Errno> BSD_USA::SendImpl(s32 fd, u32 flags, std::span<const u8> message) {
    const auto descriptor = CopyDescriptor(fd);
    if (!descriptor) {
        return {-1, Errno::BADF};
    }
    return Translate(descriptor->socket->Send(message, flags));
}

std::pair<s32, Errno> BSD_USA::SendToImpl(s32 fd, u32 flags, std::span<const u8> message,
                                      std::span<const u8> addr) {
    const auto descriptor = CopyDescriptor(fd);
    if (!descriptor) {
        return {-1, Errno::BADF};
    }

    Network::SockAddrIn addr_in;
    Network::SockAddrIn* p_addr_in = nullptr;
    if (!addr.empty()) {
        ASSERT(addr.size() >= 16);
        auto guest_addr_in = GetValue<SockAddrIn>(addr);
        addr_in = Translate(guest_addr_in);
        p_addr_in = &addr_in;
    }

    return Translate(descriptor->socket->SendTo(flags, message, p_addr_in));
}

std::optional<std::pair<s32, Errno>> BSD_USA::RecvMMsgImpl(HLERequestContext& ctx, s32 fd, u32 vlen,
                                                           u32 flags, s64 timeout_seconds,
                                                           s64 timeout_nanoseconds,
                                                           std::vector<u8>& messages) {
    // A call retried after waiting goes on from the messages it received, while its descriptor
    // has not closed since.
    std::optional<SelectRequest> retried = select_notifier->Take(ctx);
    if (vlen == 0) {
        return std::pair<s32, Errno>{0, Errno::SUCCESS};
    }
    if (timeout_seconds < 0 || timeout_nanoseconds < 0 || timeout_nanoseconds > 999'999'999) {
        return std::pair<s32, Errno>{-1, Errno::INVAL};
    }

    // The messages received before the call waited, and the reply holding them.
    u32 received = retried ? retried->messages_done : 0;
    std::vector<u8> reply = retried ? std::move(retried->reply) : std::vector<u8>{};
    // As with recvmmsg, a failure after the first message reports the messages already received.
    const auto finish = [&received, &reply, &messages](Errno bsd_errno) -> std::pair<s32, Errno> {
        if (received == 0) {
            return {-1, bsd_errno};
        }
        messages = std::move(reply);
        return {static_cast<s32>(received), Errno::SUCCESS};
    };

    u64 generation = 0;
    const auto descriptor = CopyFileDescriptor(fd, &generation);
    if (!descriptor || !descriptor->socket ||
        (retried && generation != retried->file_descriptor_generation)) {
        return finish(Errno::BADF);
    }

    // MSG_PEEK goes to the receives, and MSG_DONTWAIT and MSG_WAITFORONE decide which of them may
    // wait. Any other flag would change what is received, so it fails the call before any.
    using Network::FLAG_MSG_DONTWAIT;
    using Network::FLAG_MSG_PEEK;
    if ((flags & ~(FLAG_MSG_PEEK | FLAG_MSG_DONTWAIT | FLAG_MSG_WAITFORONE)) != 0) {
        LOG_WARNING(Service, "RecvMMsg flags={:#x} are not supported, fd={}", flags, fd);
        return finish(Errno::OPNOTSUPP);
    }

    // The request has SendMMsgImpl's layout. Each message's iovs are the buffers its data is
    // received into; the name, control data, msg_flags and msg_len are written by the reply.
    struct Message {
        SerializedMessage fields;
        std::vector<std::span<u8>> iovs;
        size_t capacity = 0;
    };
    MessageParser parser{messages};
    if (!parser.Take(1)) {
        return finish(Errno::INVAL);
    }
    // Each message takes at least its size fields from the buffer, so a vlen past what the buffer
    // holds fails the parse rather than growing this past the buffer's size.
    std::vector<Message> parsed;
    for (u32 index = 0; index < vlen; ++index) {
        Message& message = parsed.emplace_back();
        const auto fields = parser.TakeMessage(message.iovs);
        if (!fields) {
            return finish(Errno::INVAL);
        }
        message.fields = *fields;
        for (const std::span<u8> iov : message.iovs) {
            message.capacity += iov.size();
        }
    }

    // The reply serializes the received messages in the request's layout. The client reads it in
    // order and takes each name and control length from it, so those may shrink: the name to the
    // source address, and the control data to nothing, as none is received.
    if (reply.empty()) {
        reply.reserve(messages.size());
        reply.push_back(messages.front());
    }
    const auto append = [&reply](const void* bytes, size_t size) {
        const u8* const begin = static_cast<const u8*>(bytes);
        reply.insert(reply.end(), begin, begin + size);
    };

    // Every receive is tried without waiting, whatever the socket's mode: a wait would hold one of
    // the few threads of the service, beyond the reach of cancellation, so the call is deferred
    // instead.
    const int receive_flags = static_cast<int>((flags & FLAG_MSG_PEEK) | FLAG_MSG_DONTWAIT);
    const bool blocking = IsBlocking(fd, *descriptor);
    // Several iovs take their data through this buffer, scattered after the receive.
    std::vector<u8> scattered;
    const std::pair<s32, Errno> result = [&]() -> std::pair<s32, Errno> {
        for (; received < vlen; ++received) {
            const Message& message = parsed[received];
            std::span<u8> data;
            if (message.iovs.size() == 1) {
                data = message.iovs.front();
            } else {
                scattered.resize(message.capacity);
                data = scattered;
            }

            // Connection based sockets (e.g. TCP) report no source address.
            Network::SockAddrIn source{};
            u32 msg_flags = 0;
            const auto [ret, recv_errno] = Translate(descriptor->socket->RecvMsg(
                receive_flags, data, descriptor->is_connection_based ? nullptr : &source,
                msg_flags));
            if (ret < 0) {
                return {-1, recv_errno};
            }

            if (message.iovs.size() > 1) {
                size_t copied = 0;
                for (const std::span<u8> iov : message.iovs) {
                    if (copied == static_cast<size_t>(ret)) {
                        break;
                    }
                    const size_t count =
                        (std::min)(iov.size(), static_cast<size_t>(ret) - copied);
                    std::memcpy(iov.data(), scattered.data() + copied, count);
                    copied += count;
                }
            }

            // The source address as the kernel puts it, cut to the name buffer.
            SockAddrIn guest_source{};
            u32 name_size = 0;
            if (!descriptor->is_connection_based) {
                guest_source = Translate(source);
                name_size = static_cast<u32>((std::min)(
                    message.fields.name.size(), static_cast<size_t>(guest_source.len)));
            }
            append(&name_size, sizeof(name_size));
            append(&guest_source, name_size);

            const u32 iov_count = static_cast<u32>(message.iovs.size());
            append(&iov_count, sizeof(iov_count));
            for (const std::span<u8> iov : message.iovs) {
                const u64 iov_size = iov.size();
                append(&iov_size, sizeof(iov_size));
                append(iov.data(), iov.size());
            }

            // Control size, msg_flags, which tell a truncated datagram, and msg_len.
            const std::array<u32, 3> trailer{0, msg_flags, static_cast<u32>(ret)};
            append(trailer.data(), sizeof(trailer));
        }
        return {static_cast<s32>(received), Errno::SUCCESS};
    }();
    if (result.second != Errno::AGAIN) {
        // Every message was received, or a receive failed.
        return finish(result.second);
    }

    // As FreeBSD's recvmmsg, a timeout bounds the wait for the first message whatever the
    // socket's mode, and one that passes receives nothing. The client sends zeros for a null
    // timeout, which leaves that wait to the socket's mode, as any later one unless MSG_WAITFORONE.
    const bool timed = timeout_seconds != 0 || timeout_nanoseconds != 0;
    const bool socket_waits = blocking && (flags & FLAG_MSG_DONTWAIT) == 0;
    const bool may_wait = received == 0
                              ? timed || socket_waits
                              : socket_waits && (flags & FLAG_MSG_WAITFORONE) == 0;
    if (!may_wait) {
        return finish(Errno::AGAIN);
    }
    SelectClock::time_point deadline;
    if (retried && retried->messages_done == received) {
        // Still the wait the call was deferred for.
        deadline = retried->deadline;
    } else if (received == 0 && timed) {
        const auto now = SelectClock::now();
        // A timeout longer than the clock counts never passes.
        const auto max_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                     SelectClock::time_point::max() - now)
                                     .count() -
                                 1;
        deadline = timeout_seconds >= max_seconds
                       ? SelectClock::time_point::max()
                       : now + std::chrono::duration_cast<SelectClock::duration>(
                                   std::chrono::seconds{timeout_seconds} +
                                   std::chrono::nanoseconds{timeout_nanoseconds});
    } else {
        // As SO_RCVTIMEO bounds a receive.
        deadline = TimeoutDeadline(descriptor->receive_timeout);
    }
    if (SelectClock::now() >= deadline) {
        // The first message's timeout passing receives nothing; SO_RCVTIMEO passing fails the
        // receive with EAGAIN.
        if (received == 0 && timed) {
            return std::pair<s32, Errno>{0, Errno::SUCCESS};
        }
        return finish(Errno::AGAIN);
    }
    SelectRequest request{
        .deadline = deadline,
        .messages_done = received,
        .reply = std::move(reply),
    };
    if (!DeferSocketWait(ctx, fd, *descriptor, generation, false, request)) {
        // Requests can no longer wait, as emulation stops. Nothing was deferred.
        reply = std::move(request.reply);
        return finish(Errno::INTR);
    }
    return std::nullopt;
}

std::optional<std::pair<s32, Errno>> BSD_USA::SendMMsgImpl(HLERequestContext& ctx, s32 fd, u32 vlen,
                                                           u32 flags, std::vector<u8>& messages) {
    // A call retried after waiting goes on with the message it waited to send, while its
    // descriptor has not closed since.
    const std::optional<SelectRequest> retried = select_notifier->Take(ctx);
    if (vlen == 0) {
        return std::pair<s32, Errno>{0, Errno::SUCCESS};
    }
    // The messages sent before the call waited, and how much a stream sent of the next one.
    const u32 done = retried ? retried->messages_done : 0;
    size_t offset = retried ? retried->message_offset : 0;

    u64 generation = 0;
    const auto descriptor = CopyFileDescriptor(fd, &generation);
    const bool closed = !descriptor || !descriptor->socket ||
                        (retried && generation != retried->file_descriptor_generation);
    if (closed && done == 0) {
        return std::pair<s32, Errno>{-1, Errno::BADF};
    }

    // MSG_DONTWAIT has no send wait, and MSG_NOSIGNAL asks for nothing. Any other flag would
    // change what is sent, so it fails the call before any.
    using Network::FLAG_MSG_DONTWAIT;
    if ((flags & ~(FLAG_MSG_DONTWAIT | FLAG_MSG_NOSIGNAL)) != 0) {
        LOG_WARNING(Service, "SendMMsg flags={:#x} are not supported, fd={}", flags, fd);
        return std::pair<s32, Errno>{-1, Errno::OPNOTSUPP};
    }

    u32 sent = 0;
    // As with sendmmsg, a failure after the first message reports the messages already sent.
    const auto fail = [&sent](Errno bsd_errno) -> std::pair<s32, Errno> {
        if (sent > 0) {
            return {static_cast<s32>(sent), Errno::SUCCESS};
        }
        return {-1, bsd_errno};
    };

    // Layout: a header byte the system ignores, then the messages (see MessageParser).
    MessageParser parser{messages};
    if (!parser.Take(1)) {
        return fail(Errno::INVAL);
    }
    std::vector<std::span<u8>> iovs;
    // The messages sent before the call waited went whole.
    for (; sent < done; ++sent) {
        const auto fields = parser.TakeMessage(iovs);
        if (!fields) {
            return fail(Errno::INVAL);
        }
        size_t size = 0;
        for (const std::span<u8> iov : iovs) {
            size += iov.size();
        }
        PutValue(fields->msg_len, static_cast<u32>(size));
    }
    if (closed) {
        return fail(Errno::BADF);
    }

    // Why a well-formed message cannot be sent, or SUCCESS.
    const auto check = [](const SerializedMessage& message) {
        if (!message.control.empty()) {
            return Errno::OPNOTSUPP;
        }
        if (message.name.empty()) {
            return Errno::SUCCESS;
        }
        // An IPv4 address takes 16 bytes, as in a sockaddr_in; up to the whole storage is accepted.
        if (message.name.size() < 16 || message.name.size() > sizeof(SockAddrIn)) {
            return Errno::INVAL;
        }
        // Only IPv4 translates: an address of another family fails, as SocketImpl does, rather
        // than being sent to as an IPv4 address.
        if (static_cast<Domain>(GetValue<SockAddrIn>(message.name).family) != Domain::INET) {
            return Errno::AFNOSUPPORT;
        }
        return Errno::SUCCESS;
    };

    // Every send is tried without waiting, whatever the socket's mode: a wait would hold one of
    // the few threads of the service, beyond the reach of cancellation, so the call is deferred
    // instead. A single iov is sent in place; several are gathered into one buffer, reused by the
    // next message.
    const bool blocking = IsBlocking(fd, *descriptor);
    std::vector<u8> gathered;
    // The msg_len of the message being sent.
    std::span<u8> msg_len;
    const std::pair<s32, Errno> result = [&]() -> std::pair<s32, Errno> {
        for (; sent < vlen; ++sent, offset = 0) {
            const auto fields = parser.TakeMessage(iovs);
            if (!fields) {
                return {-1, Errno::INVAL};
            }
            if (const Errno check_errno = check(*fields); check_errno != Errno::SUCCESS) {
                if (check_errno == Errno::OPNOTSUPP) {
                    LOG_WARNING(Service, "SendMMsg control data is not supported, fd={} size={}",
                                fd, fields->control.size());
                }
                return {-1, check_errno};
            }
            msg_len = fields->msg_len;

            std::span<const u8> data;
            if (iovs.size() == 1) {
                data = iovs.front();
            } else {
                gathered.clear();
                for (const std::span<u8> iov : iovs) {
                    gathered.insert(gathered.end(), iov.begin(), iov.end());
                }
                data = gathered;
            }
            // What a stream sent of the message before the call waited is not sent again.
            offset = (std::min)(offset, data.size());
            const std::span<const u8> rest = data.subspan(offset);
            const auto [ret, send_errno] = [&] {
                if (fields->name.empty()) {
                    return Translate(descriptor->socket->Send(rest, FLAG_MSG_DONTWAIT));
                }
                const Network::SockAddrIn addr_in = Translate(GetValue<SockAddrIn>(fields->name));
                return Translate(descriptor->socket->SendTo(FLAG_MSG_DONTWAIT, rest, &addr_in));
            }();
            if (ret < 0) {
                return {-1, send_errno};
            }
            offset += static_cast<size_t>(ret);
            PutValue(msg_len, static_cast<u32>(offset));
            if (offset < data.size()) {
                // A stream sends the rest once it can, as a blocking send would wait to.
                if (ret > 0 && descriptor->is_connection_based) {
                    return {-1, Errno::AGAIN};
                }
                // The unsent rest of a message must go before any later message, so what
                // another socket sent of it ends the call.
                ++sent;
                return {static_cast<s32>(sent), Errno::SUCCESS};
            }
        }
        return {static_cast<s32>(sent), Errno::SUCCESS};
    }();
    if (result.second != Errno::AGAIN) {
        // Every message was sent, a short one ended the call, or a send failed.
        return result.second == Errno::SUCCESS ? result : fail(result.second);
    }

    // A send that would wait: one of a blocking socket waits, until SO_SNDTIMEO or the end of
    // emulation, which, as for sendmsg, counts what a stream sent of the message.
    const auto end_wait = [&](Errno bsd_errno) -> std::pair<s32, Errno> {
        if (offset > 0) {
            PutValue(msg_len, static_cast<u32>(offset));
            ++sent;
        }
        return fail(bsd_errno);
    };
    if (!blocking || (flags & FLAG_MSG_DONTWAIT) != 0) {
        return end_wait(Errno::AGAIN);
    }
    // Still the wait the call was deferred for, or one for what the stream sent since.
    const SelectClock::time_point deadline =
        retried && retried->messages_done == sent && retried->message_offset == offset
            ? retried->deadline
            : TimeoutDeadline(descriptor->send_timeout);
    if (SelectClock::now() >= deadline) {
        return end_wait(Errno::AGAIN);
    }
    SelectRequest request{
        .deadline = deadline,
        .messages_done = sent,
        .message_offset = offset,
    };
    if (!DeferSocketWait(ctx, fd, *descriptor, generation, true, request)) {
        // Requests can no longer wait, as emulation stops. Nothing was deferred.
        return end_wait(Errno::INTR);
    }
    return std::nullopt;
}

template <typename Operation>
std::optional<std::pair<s32, Errno>> BSD_USA::EventFdIoImpl(HLERequestContext& ctx, s32 fd,
                                                            const FileDescriptor& descriptor,
                                                            u64 generation, Operation operation) {
    u64 blocked_version = 0;
    const std::pair<s32, Errno> result = operation(blocked_version);
    if (result.second == Errno::SUCCESS) {
        // The counter changed under the requests waiting on the eventfd.
        select_notifier->NotifyChange();
        return result;
    }
    if (result.second != Errno::AGAIN || descriptor.event_fd->IsNonBlocking()) {
        return result;
    }
    // Waiting here would hold one of the few threads of the service, which the read, write or
    // close that ends the wait may need. The request is deferred instead: it is retried once the
    // counter's version moves on from the one found here, which closing a descriptor of the
    // eventfd also does, or fd no longer refers to the same descriptor.
    SelectRequest request{
        .file_descriptor_generation = generation,
    };
    SelectWait wait{
        .entries = {SelectWait::Entry{
            .fd = fd,
            .socket = descriptor.socket,
            .event_fd = descriptor.event_fd,
            .event_fd_version = blocked_version,
            .generation = generation,
        }},
    };
    if (!select_notifier->Defer(ctx, request, wait)) {
        // Requests can no longer wait, as emulation stops.
        return std::pair<s32, Errno>{-1, Errno::INTR};
    }
    ctx.SetIsDeferred();
    return std::nullopt;
}

std::optional<std::pair<s32, Errno>> BSD_USA::ReadImpl(HLERequestContext& ctx, s32 fd,
                                                       std::vector<u8>& message) {
    // A read retried after waiting goes on only while its descriptor has not closed since, even
    // should the same eventfd or socket have been put back in the slot.
    const std::optional<SelectRequest> retried = select_notifier->Take(ctx);
    u64 generation = 0;
    const auto descriptor = CopyFileDescriptor(fd, &generation);
    if (!descriptor || (retried && generation != retried->file_descriptor_generation)) {
        return std::pair<s32, Errno>{-1, Errno::BADF};
    }
    if (descriptor->event_fd) {
        return EventFdIoImpl(ctx, fd, *descriptor, generation, [&](u64& blocked_version) {
            return descriptor->event_fd->Read(message, blocked_version);
        });
    }
    // On a socket, read is recv without flags. Waiting on a blocking socket would hold one of the
    // few threads of the service, beyond the reach of cancellation; the read is tried without
    // waiting and deferred instead, until the socket's receive timeout, which then fails it with
    // EAGAIN, as SO_RCVTIMEO does.
    const bool blocking = IsBlocking(fd, *descriptor);
    const std::pair<s32, Errno> result =
        Translate(descriptor->socket->Recv(Network::FLAG_MSG_DONTWAIT, message));
    if (!blocking || result.second != Errno::AGAIN) {
        return result;
    }
    SelectRequest request{
        .deadline = retried ? retried->deadline : TimeoutDeadline(descriptor->receive_timeout),
    };
    if (SelectClock::now() >= request.deadline) {
        return result;
    }
    if (!DeferSocketWait(ctx, fd, *descriptor, generation, false, request)) {
        // Requests can no longer wait, as emulation stops.
        return std::pair<s32, Errno>{-1, Errno::INTR};
    }
    return std::nullopt;
}

std::optional<std::pair<s32, Errno>> BSD_USA::WriteImpl(HLERequestContext& ctx, s32 fd,
                                                        std::span<const u8> message) {
    // A write retried after waiting on an eventfd goes on only while its descriptor has not
    // closed since, even should the same eventfd have been put back in the slot.
    const std::optional<SelectRequest> retried = select_notifier->Take(ctx);
    u64 generation = 0;
    const auto descriptor = CopyFileDescriptor(fd, &generation);
    if (!descriptor || (retried && generation != retried->file_descriptor_generation)) {
        return std::pair<s32, Errno>{-1, Errno::BADF};
    }
    if (descriptor->event_fd) {
        return EventFdIoImpl(ctx, fd, *descriptor, generation, [&](u64& blocked_version) {
            return descriptor->event_fd->Write(message, blocked_version);
        });
    }
    // On a socket, write is send without flags.
    return Translate(descriptor->socket->Send(message, 0));
}

std::pair<s32, Errno> BSD_USA::EventFdImpl(u64 initval, u32 flags) {
    if (initval > EventFdState::MAX_COUNTER) {
        return {-1, Errno::INVAL};
    }
    constexpr u32 known_flags = EventFdState::FLAG_SEMAPHORE | EventFdState::FLAG_NONBLOCK;
    if ((flags & ~known_flags) != 0) {
        LOG_WARNING(Service, "Ignoring unknown eventfd flags={:#x}", flags & ~known_flags);
    }

    auto state = std::make_shared<EventFdState>();
    state->counter = initval;
    state->semaphore = (flags & EventFdState::FLAG_SEMAPHORE) != 0;
    if ((flags & EventFdState::FLAG_NONBLOCK) != 0) {
        state->status_flags.store(static_cast<s32>(Network::FLAG_O_NONBLOCK),
                                  std::memory_order_relaxed);
    }

    std::scoped_lock lock{file_descriptors_mutex};
    const s32 fd = FindFreeFileDescriptorHandle();
    if (fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return {-1, Errno::MFILE};
    }
    file_descriptors[fd] = FileDescriptor{
        .event_fd = std::move(state),
    };

    LOG_INFO(Service, "New eventfd fd={}", fd);
    return {fd, Errno::SUCCESS};
}

Errno BSD_USA::CloseImpl(s32 fd) {
    FileDescriptor descriptor;
    // Whether another descriptor shares the eventfd, which keeps it open.
    bool event_fd_shared = false;
    {
        // Free the slot first, so of two threads closing the same fd only one closes the socket.
        // Like POSIX close(), the fd is released even if closing the socket fails.
        std::scoped_lock lock{file_descriptors_mutex};
        if (!IsFileDescriptorValid(fd)) {
            return Errno::BADF;
        }
        descriptor = std::move(*file_descriptors[fd]);
        file_descriptors[fd].reset();
        // A request deferred on the descriptor tells it from a later one in the slot by the
        // generation, and the waits taken on an eventfd end with its version, which a duplicate
        // of the eventfd put back in the slot would otherwise keep current.
        ++file_descriptor_generations[fd];
        if (descriptor.event_fd) {
            descriptor.event_fd->DescriptorClosed();
        }
        // An eventfd stays open until its last duplicate closes, while a socket closes for every
        // duplicate sharing it, which then reads as closed: this thread alone closes it.
        for (std::optional<FileDescriptor>& other : file_descriptors) {
            if (!other) {
                continue;
            }
            if (descriptor.event_fd) {
                event_fd_shared |= other->event_fd == descriptor.event_fd;
            } else if (descriptor.socket && other->socket == descriptor.socket) {
                other->socket_closed = true;
            }
        }
    }

    if (descriptor.event_fd) {
        LOG_INFO(Service, "Close eventfd fd={}", fd);
        // Deferred Selects and Polls find the descriptor closed, and reads and writes on it fail.
        select_notifier->NotifyChange();
        if (!event_fd_shared) {
            descriptor.event_fd->Close();
        }
        return Errno::SUCCESS;
    }
    if (!descriptor.socket) {
        // An accepted room connection has no socket, as room sockets do not accept.
        LOG_WARNING(Service, "Close fd={}, which had no socket", fd);
        return Errno::SUCCESS;
    }

    LOG_INFO(Service, "Close socket fd={}", fd);
    // Deferred Selects and Polls let go of the host socket before it closes.
    select_notifier->NotifyChange(descriptor.socket.get());
    // Closing a duplicate closed it already.
    if (descriptor.socket_closed) {
        return Errno::SUCCESS;
    }
    return Translate(descriptor.socket->Close());
}

std::variant<s32, Errno> BSD_USA::DuplicateSocketImpl(s32 fd) {
    std::scoped_lock lock{file_descriptors_mutex};
    const FileDescriptor* const descriptor = FindOpenDescriptor(fd);
    if (descriptor == nullptr) {
        return Errno::BADF;
    }

    const s32 new_fd = FindFreeFileDescriptorHandle();
    if (new_fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return Errno::MFILE;
    }

    // The duplicate shares the socket or eventfd, and copies the socket's flags.
    file_descriptors[new_fd] = *descriptor;
    return new_fd;
}

std::optional<std::shared_ptr<Network::SocketBase>> BSD_USA::GetSocket(s32 fd) {
    if (auto descriptor = CopyDescriptor(fd)) {
        return std::move(descriptor->socket);
    }
    return std::nullopt;
}

const BSD_USA::FileDescriptor* BSD_USA::FindOpenDescriptor(s32 fd) noexcept {
    if (fd < 0 || fd >= static_cast<s32>(MAX_FD) || !file_descriptors[fd]) {
        return nullptr;
    }
    const FileDescriptor& descriptor = *file_descriptors[fd];
    const bool open = descriptor.event_fd || (descriptor.socket && !descriptor.socket_closed);
    return open ? &descriptor : nullptr;
}

std::optional<BSD_USA::FileDescriptor> BSD_USA::CopyFileDescriptor(s32 fd, u64* generation) {
    // The copy holds a reference to the socket or eventfd, so a close on another bsd thread
    // cannot destroy it while the caller uses it
    std::scoped_lock lock{file_descriptors_mutex};
    if (!IsFileDescriptorValid(fd)) {
        return std::nullopt;
    }
    const FileDescriptor* const descriptor = FindOpenDescriptor(fd);
    if (descriptor == nullptr) {
        LOG_WARNING(Service, "File descriptor handle={} is not open", fd);
        return std::nullopt;
    }
    if (generation != nullptr) {
        *generation = file_descriptor_generations[fd];
    }
    return *descriptor;
}

std::optional<BSD_USA::FileDescriptor> BSD_USA::CopyDescriptor(s32 fd) {
    auto descriptor = CopyFileDescriptor(fd);
    if (descriptor && !descriptor->socket) {
        LOG_ERROR(Service, "File descriptor handle={} is an eventfd, not a socket", fd);
        return std::nullopt;
    }
    return descriptor;
}

s32 BSD_USA::FindFreeFileDescriptorHandle() noexcept {
    for (s32 fd = 0; fd < static_cast<s32>(file_descriptors.size()); ++fd) {
        if (!file_descriptors[fd]) {
            return fd;
        }
    }
    return -1;
}

bool BSD_USA::IsBlocking(s32 fd, const FileDescriptor& descriptor) {
    // Read the current flags, not the copy: fcntl may have changed them since it was taken
    std::scoped_lock lock{file_descriptors_mutex};
    const auto& entry = file_descriptors[fd];
    const s32 current_flags =
        entry && entry->socket == descriptor.socket ? entry->flags : descriptor.flags;
    return (current_flags & Network::FLAG_O_NONBLOCK) == 0;
}

bool BSD_USA::IsFileDescriptorValid(s32 fd) const noexcept {
    if (fd >= static_cast<s32>(MAX_FD) || fd < 0) {
        LOG_ERROR(Service, "Invalid file descriptor handle={}", fd);
        return false;
    }
    if (!file_descriptors[fd]) {
        LOG_ERROR(Service, "File descriptor handle={} is not allocated", fd);
        return false;
    }
    return true;
}

void BSD_USA::BuildErrnoResponse(HLERequestContext& ctx, Errno bsd_errno) const noexcept {
    IPC::ResponseBuilder rb{ctx, 4};

    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(bsd_errno);
}

bool BSD_USA::OnProxyPacketReceived(const Network::ProxyPacket& packet) {
    // Runs on the room thread. The copies keep each socket alive if a bsd thread closes it, so
    // the bsd threads do not wait for the decompression below. Only sockets that accept the
    // packet are collected, so an unwanted packet is never decompressed.
    std::array<std::shared_ptr<Network::SocketBase>, MAX_FD> sockets{};
    size_t num_sockets = 0;
    {
        std::scoped_lock lock{file_descriptors_mutex};
        for (const auto& descriptor : file_descriptors) {
            if (!descriptor.has_value() || !descriptor->socket) {
                continue;
            }
            // A duplicated fd shares its socket, which must still get each packet only once
            const auto sockets_end = sockets.begin() + num_sockets;
            if (std::find(sockets.begin(), sockets_end, descriptor->socket) == sockets_end &&
                descriptor->socket->AcceptsProxyPacket(packet)) {
                sockets[num_sockets++] = descriptor->socket;
            }
        }
    }
    if (num_sockets == 0) {
        return false;
    }

    // Decompress once for all sockets that share the port. The room does not check the frame, so
    // drop a malformed or oversized one instead of throwing on the room thread.
    auto data = Common::Compression::TryDecompressDataZSTD(packet.data, MAX_ROOM_PACKET_SIZE);
    if (!data) {
        LOG_WARNING(Service, "Dropped a room packet with an invalid or oversized payload");
        return false;
    }
    const Network::ProxyPacket decompressed{
        .local_endpoint = packet.local_endpoint,
        .remote_endpoint = packet.remote_endpoint,
        .protocol = packet.protocol,
        .broadcast = packet.broadcast,
        .data = std::move(*data),
    };
    for (size_t i = 0; i < num_sockets; ++i) {
        sockets[i]->HandleProxyPacket(decompressed);
    }
    return true;
}

BSD_USA::BSD_USA(Core::System& system_, const char* name, bool is_user_,
                 std::shared_ptr<SelectNotifier> select_notifier_)
    : ServiceFramework{system_, name}
    , select_notifier{std::move(select_notifier_)}
    , is_user{is_user_} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &BSD_USA::RegisterClient, "RegisterClient"},
        {1, &BSD_USA::StartMonitoring, "StartMonitoring"},
        {2, &BSD_USA::Socket, "Socket"},
        {3, &BSD_USA::SocketExempt, "SocketExempt"},
        {4, &BSD_USA::StubReplyZeroed, "Open"},
        {5, &BSD_USA::Select, "Select"},
        {6, &BSD_USA::Poll, "Poll"},
        {7, &BSD_USA::StubReplyZeroed, "Sysctl"},
        {8, &BSD_USA::Recv, "Recv"},
        {9, &BSD_USA::RecvFrom, "RecvFrom"},
        {10, &BSD_USA::Send, "Send"},
        {11, &BSD_USA::SendTo, "SendTo"},
        {12, &BSD_USA::Accept, "Accept"},
        {13, &BSD_USA::Bind, "Bind"},
        {14, &BSD_USA::Connect, "Connect"},
        {15, &BSD_USA::GetPeerName, "GetPeerName"},
        {16, &BSD_USA::GetSockName, "GetSockName"},
        {17, &BSD_USA::GetSockOpt, "GetSockOpt"},
        {18, &BSD_USA::Listen, "Listen"},
        {19, &BSD_USA::StubReplyZeroed, "Ioctl"},
        {20, &BSD_USA::Fcntl, "Fcntl"},
        {21, &BSD_USA::SetSockOpt, "SetSockOpt"},
        {22, &BSD_USA::Shutdown, "Shutdown"},
        {23, &BSD_USA::StubReplyZeroed, "ShutdownAllSockets"},
        {24, &BSD_USA::Write, "Write"},
        {25, &BSD_USA::Read, "Read"},
        {26, &BSD_USA::Close, "Close"},
        {27, &BSD_USA::DuplicateSocket, "DuplicateSocket"},
        {28, &BSD_USA::StubReplyZeroed, "GetResourceStatistics"},
        {29, &BSD_USA::RecvMMsg, "RecvMMsg"}, //3.0.0+
        {30, &BSD_USA::SendMMsg, "SendMMsg"}, //3.0.0+
        {31, &BSD_USA::EventFd, "EventFd"}, //7.0.0+
        {32, &BSD_USA::StubReplyZeroed, "RegisterResourceStatisticsName"}, //7.0.0+
        {33, &BSD_USA::StubReplyZeroed, "RegisterClientShared"}, //10.0.0+
        {34, &BSD_USA::StubReplyZeroed, "GetSocketStatistics"}, //15.0.0+
        {35, &BSD_USA::StubReplyZeroed, "NifIoctl"}, //17.0.0+
        // Switch 2 titles register with 36 (16 bytes and the pid; a bsd errno back) instead of
        // RegisterClient. The auto stub left the errno to the message buffer's leftovers, and
        // Minecraft's socket layer failed later in EventFd.
        {36, &BSD_USA::RegisterClient, "RegisterClientSwitch2"}, //18.0.0+
        {37, &BSD_USA::StubReplyZeroed, "Unknown37"}, //18.0.0+
        {38, &BSD_USA::StubReplyZeroed, "Unknown38"}, //18.0.0+
        {39, &BSD_USA::StubReplyZeroed, "Unknown39"}, //20.0.0+
        {40, &BSD_USA::StubReplyZeroed, "Unknown40"}, //20.0.0+
        {41, &BSD_USA::StubReplyEvent, "Unknown41"}, //21.0.0+
        {42, &BSD_USA::StubReplyZeroed, "Unknown42"}, //21.0.0+
        {43, &BSD_USA::StubReplyZeroed, "Unknown43"}, //21.0.0+
        {200, &BSD_USA::StubReplyZeroed, "SetThreadCoreMask"}, //15.0.0+
        {201, &BSD_USA::StubReplyZeroed, "GetThreadCoreMask"}, //15.0.0+
        {44, &BSD_USA::StubReplyZeroed, "Unknown44"},
    };
    // clang-format on

    RegisterHandlers(functions);

    std::scoped_lock lock{proxy_binding_mutex};
    if (proxy_binding_count++ != 0) {
        return;
    }
    if (auto room_member = Network::GetRoomMember().lock()) {
        // A queued packet can make a deferred Select or Poll on a room socket ready. bsd:s, bsd:u
        // and bsd:a share one notifier, which outlives the binding: the last service to go
        // unbinds before it lets go of the notifier, and Unbind waits for a running callback.
        proxy_packet_received = room_member->BindOnProxyPacketReceived(
            [notifier = select_notifier.get()](const Network::ProxyPacket& packet) {
                if (OnProxyPacketReceived(packet)) {
                    notifier->NotifyChange();
                }
            });
    } else {
        LOG_ERROR(Service, "Network isn't initialized");
    }
}

BSD_USA::~BSD_USA() {
    std::scoped_lock lock{proxy_binding_mutex};
    if (--proxy_binding_count != 0) {
        return;
    }
    if (auto room_member = Network::GetRoomMember().lock()) {
        room_member->Unbind(proxy_packet_received);
    }
    proxy_packet_received.reset();
}

std::unique_lock<std::mutex> BSD_USA::LockService() noexcept {
    return {};
}

BSDCFG::BSDCFG(Core::System& system_, const char *name)
    : ServiceFramework{system_, name} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &BSDCFG::StubReplyZeroed, "SetIfUp"},
        {1, &BSDCFG::StubReplyZeroed, "SetIfUpWithEvent"},
        {2, &BSDCFG::StubReplyZeroed, "CancelIf"},
        {3, &BSDCFG::StubReplyZeroed, "SetIfDown"},
        {4, &BSDCFG::StubReplyZeroed, "GetIfState"},
        {5, &BSDCFG::StubReplyZeroed, "DhcpRenew"},
        {6, &BSDCFG::StubReplyZeroed, "AddStaticArpEntry"},
        {7, &BSDCFG::StubReplyZeroed, "RemoveArpEntry"},
        {8, &BSDCFG::StubReplyZeroed, "LookupArpEntry"},
        {9, &BSDCFG::StubReplyZeroed, "LookupArpEntry2"},
        {10, &BSDCFG::StubReplyZeroed, "ClearArpEntries"},
        {11, &BSDCFG::StubReplyZeroed, "ClearArpEntries2"},
        {12, &BSDCFG::StubReplyZeroed, "PrintArpEntries"},
        {13, &BSDCFG::StubReplyZeroed, "Unknown13"},
        {14, &BSDCFG::StubReplyZeroed, "Unknown14"},
        {15, &BSDCFG::StubReplyZeroed, "Unknown15"},
        {16, &BSDCFG::StubReplyZeroed, "Unknown16"},
        {100, &BSDCFG::StubReplyZeroed, "Unknown100"},
        {101, &BSDCFG::StubReplyZeroed, "Unknown101"},
        {102, &BSDCFG::StubReplyZeroed, "Unknown102"},
        {103, &BSDCFG::StubReplyZeroed, "Unknown103"},
        {104, &BSDCFG::StubReplyZeroed, "Unknown104"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

BSDCFG::~BSDCFG() = default;

BSD_NU::BSD_NU(Core::System& system_)
    : ServiceFramework{system_, "bsd:nu"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "CreateUserService"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

BSD_NU::~BSD_NU() = default;

} // namespace Service::Sockets
