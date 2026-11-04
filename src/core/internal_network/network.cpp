// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "common/error.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/internal_network/network.h"
#include "core/internal_network/network_interface.h"
#include "core/internal_network/sockets.h"
#include "network/network.h"

namespace Network {

namespace {

enum class CallType {
    Send,
    Other,
};

/// Set while socket operations are cancelled, for waits that no host socket operation wakes.
std::atomic_bool socket_operations_cancelled{false};

#ifdef _WIN32

using socklen_t = int;

SOCKET interrupt_socket = static_cast<SOCKET>(-1);

void InterruptSocketOperations() {
    closesocket(interrupt_socket);
}

void AcknowledgeInterrupt() {
    interrupt_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}

void Initialize() {
    WSADATA wsa_data;
    (void)WSAStartup(MAKEWORD(2, 2), &wsa_data);

    AcknowledgeInterrupt();
}

void Finalize() {
    InterruptSocketOperations();
    WSACleanup();
}

SOCKET GetInterruptSocket() {
    return interrupt_socket;
}

sockaddr TranslateFromSockAddrIn(SockAddrIn input) {
    sockaddr_in result;

#ifdef __unix__
    result.sin_len = sizeof(result);
#endif

    switch (static_cast<Domain>(input.family)) {
    case Domain::INET:
        result.sin_family = AF_INET;
        break;
    default:
        UNIMPLEMENTED_MSG("Unhandled sockaddr family={}", input.family);
        result.sin_family = AF_INET;
        break;
    }

    result.sin_port = htons(input.portno);

    auto& ip = result.sin_addr.S_un.S_un_b;
    ip.s_b1 = input.ip[0];
    ip.s_b2 = input.ip[1];
    ip.s_b3 = input.ip[2];
    ip.s_b4 = input.ip[3];

    sockaddr addr;
    std::memcpy(&addr, &result, sizeof(addr));
    return addr;
}

LINGER MakeLinger(bool enable, u32 linger_value) {
    ASSERT(linger_value <= (std::numeric_limits<u_short>::max)());

    LINGER value;
    value.l_onoff = enable ? 1 : 0;
    value.l_linger = static_cast<u_short>(linger_value);
    return value;
}

/// SO_RCVTIMEO and SO_SNDTIMEO take milliseconds on Windows.
DWORD MakeTimeout(u32 timeout_ms) {
    return timeout_ms;
}

bool EnableNonBlock(SOCKET fd, bool enable) {
    u_long value = enable ? 1 : 0;
    return ioctlsocket(fd, FIONBIO, &value) != SOCKET_ERROR;
}

Errno TranslateNativeError(int e, CallType call_type = CallType::Other) {
    switch (e) {
    case 0:
        return Errno::SUCCESS;
    case WSAEBADF:
        return Errno::BADF;
    case WSAEINVAL:
        return Errno::INVAL;
    case WSAEMFILE:
        return Errno::MFILE;
    case WSAENOTCONN:
        return Errno::NOTCONN;
    case WSAEWOULDBLOCK:
        return Errno::AGAIN;
    case WSAECONNREFUSED:
        return Errno::CONNREFUSED;
    case WSAECONNABORTED:
        if (call_type == CallType::Send) {
            // Winsock yields WSAECONNABORTED from `send` in situations where Unix
            // systems, and actual Switches, yield EPIPE.
            return Errno::PIPE;
        } else {
            return Errno::CONNABORTED;
        }
    case WSAECONNRESET:
        return Errno::CONNRESET;
    case WSAEHOSTUNREACH:
        return Errno::HOSTUNREACH;
    case WSAENETDOWN:
        return Errno::NETDOWN;
    case WSAENETUNREACH:
        return Errno::NETUNREACH;
    case WSAEMSGSIZE:
        return Errno::MSGSIZE;
    case WSAETIMEDOUT:
        return Errno::TIMEDOUT;
    case WSAEINPROGRESS:
        return Errno::INPROGRESS;
    case WSAEISCONN:
        return Errno::ISCONN;
    default:
        UNIMPLEMENTED_MSG("Unimplemented errno={}", e);
        return Errno::OTHER;
    }
}

#else // ^^^ Windows vvv POSIX

using SOCKET = int;
using WSAPOLLFD = pollfd;
using ULONG = u64;

constexpr SOCKET SOCKET_ERROR = -1;

constexpr int SD_RECEIVE = SHUT_RD;
constexpr int SD_SEND = SHUT_WR;
constexpr int SD_BOTH = SHUT_RDWR;

int interrupt_pipe_fd[2] = {-1, -1};

void Initialize() {
    if (pipe(interrupt_pipe_fd) != 0) {
        LOG_ERROR(Network, "Failed to create interrupt pipe!");
    }
    int flags = fcntl(interrupt_pipe_fd[0], F_GETFL);
    ASSERT_MSG(fcntl(interrupt_pipe_fd[0], F_SETFL, flags | O_NONBLOCK) == 0,
               "Failed to set nonblocking state for interrupt pipe");
}

void Finalize() {
    if (interrupt_pipe_fd[0] >= 0) {
        close(interrupt_pipe_fd[0]);
    }
    if (interrupt_pipe_fd[1] >= 0) {
        close(interrupt_pipe_fd[1]);
    }
}

void InterruptSocketOperations() {
    u8 value = 0;
    ASSERT(write(interrupt_pipe_fd[1], &value, sizeof(value)) == 1);
}

void AcknowledgeInterrupt() {
    u8 value = 0;
    ssize_t ret = read(interrupt_pipe_fd[0], &value, sizeof(value));
    if (ret != 1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_ERROR(Network, "Failed to acknowledge interrupt on shutdown");
    }
}

SOCKET GetInterruptSocket() {
    return interrupt_pipe_fd[0];
}

sockaddr TranslateFromSockAddrIn(SockAddrIn input) {
    sockaddr_in result;

    switch (static_cast<Domain>(input.family)) {
    case Domain::INET:
        result.sin_family = AF_INET;
        break;
    default:
        UNIMPLEMENTED_MSG("Unhandled sockaddr family={}", input.family);
        result.sin_family = AF_INET;
        break;
    }

    result.sin_port = htons(input.portno);

    result.sin_addr.s_addr = input.ip[0] | input.ip[1] << 8 | input.ip[2] << 16 | input.ip[3] << 24;

    sockaddr addr;
    std::memcpy(&addr, &result, sizeof(addr));
    return addr;
}

int WSAPoll(WSAPOLLFD* fds, ULONG nfds, int timeout) {
    return poll(fds, static_cast<nfds_t>(nfds), timeout);
}

int closesocket(SOCKET fd) {
    return close(fd);
}

linger MakeLinger(bool enable, u32 linger_value) {
    linger value;
    value.l_onoff = enable ? 1 : 0;
    value.l_linger = linger_value;
    return value;
}

/// SO_RCVTIMEO and SO_SNDTIMEO take a timeval elsewhere.
timeval MakeTimeout(u32 timeout_ms) {
    timeval value{};
    value.tv_sec = static_cast<decltype(value.tv_sec)>(timeout_ms / 1000);
    value.tv_usec = static_cast<decltype(value.tv_usec)>((timeout_ms % 1000) * 1000);
    return value;
}

bool EnableNonBlock(int fd, bool enable) {
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        return false;
    }
    if (enable) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return fcntl(fd, F_SETFL, flags) == 0;
}

Errno TranslateNativeError(int e, CallType call_type = CallType::Other) {
    switch (e) {
    case 0:
        return Errno::SUCCESS;
    case EBADF:
        return Errno::BADF;
    case EINVAL:
        return Errno::INVAL;
    case EMFILE:
        return Errno::MFILE;
    case EPIPE:
        return Errno::PIPE;
    case ECONNABORTED:
        return Errno::CONNABORTED;
    case ENOTCONN:
        return Errno::NOTCONN;
    case EAGAIN:
        return Errno::AGAIN;
    case ECONNREFUSED:
        return Errno::CONNREFUSED;
    case ECONNRESET:
        return Errno::CONNRESET;
    case EHOSTUNREACH:
        return Errno::HOSTUNREACH;
    case ENETDOWN:
        return Errno::NETDOWN;
    case ENETUNREACH:
        return Errno::NETUNREACH;
    case EMSGSIZE:
        return Errno::MSGSIZE;
    case ETIMEDOUT:
        return Errno::TIMEDOUT;
    case EINPROGRESS:
        return Errno::INPROGRESS;
    case EISCONN:
        return Errno::ISCONN;
    default:
        UNIMPLEMENTED_MSG("Unimplemented errno={} ({})", e, strerror(e));
        return Errno::OTHER;
    }
}

#endif

Errno GetAndLogLastError(CallType call_type = CallType::Other) {
#ifdef _WIN32
    int e = WSAGetLastError();
#else
    int e = errno;
#endif
    const Errno err = TranslateNativeError(e, call_type);
    if (err == Errno::AGAIN || err == Errno::TIMEDOUT || err == Errno::INPROGRESS) {
        // These happen during normal operation, so only log them at debug level.
        LOG_DEBUG(Network, "Socket operation error: {}", Common::NativeErrorToString(e));
        return err;
    }
    LOG_ERROR(Network, "Socket operation error: {}", Common::NativeErrorToString(e));
    return err;
}

GetAddrInfoError TranslateGetAddrInfoErrorFromNative(int gai_err) {
    switch (gai_err) {
    case 0: return GetAddrInfoError::SUCCESS;
    case EAI_AGAIN: return GetAddrInfoError::AGAIN;
    case EAI_BADFLAGS: return GetAddrInfoError::BADFLAGS;
    case EAI_FAIL: return GetAddrInfoError::FAIL;
    case EAI_FAMILY: return GetAddrInfoError::FAMILY;
    case EAI_MEMORY: return GetAddrInfoError::MEMORY;
    case EAI_NONAME: return GetAddrInfoError::NONAME;
    case EAI_SERVICE: return GetAddrInfoError::SERVICE;
    case EAI_SOCKTYPE: return GetAddrInfoError::SOCKTYPE;
    // These codes may not be defined on all systems:
#ifdef EAI_ADDRFAMILY
    case EAI_ADDRFAMILY: return GetAddrInfoError::ADDRFAMILY;
#endif
#ifdef EAI_SYSTEM
    case EAI_SYSTEM: return GetAddrInfoError::SYSTEM;
#endif
#ifdef EAI_BADHINTS
    case EAI_BADHINTS: return GetAddrInfoError::BADHINTS;
#endif
#ifdef EAI_PROTOCOL
    case EAI_PROTOCOL: return GetAddrInfoError::PROTOCOL;
#endif
#ifdef EAI_OVERFLOW
    case EAI_OVERFLOW: return GetAddrInfoError::OVERFLOW_;
#endif
    default:
#ifdef EAI_NODATA
        // This can't be a case statement because it would create a duplicate
        // case on Windows where EAI_NODATA is an alias for EAI_NONAME.
        if (gai_err == EAI_NODATA)
            return GetAddrInfoError::NODATA;
#endif
        return GetAddrInfoError::OTHER;
    }
}

Domain TranslateDomainFromNative(int domain) {
    switch (domain) {
    case 0:
        return Domain::Unspecified;
    case AF_INET:
        return Domain::INET;
    default:
        UNIMPLEMENTED_MSG("Unhandled domain={}", domain);
        return Domain::INET;
    }
}

int TranslateDomainToNative(Domain domain) {
    switch (domain) {
    case Domain::Unspecified:
        return 0;
    case Domain::INET:
        return AF_INET;
    default:
        UNIMPLEMENTED_MSG("Unimplemented domain={}", domain);
        return 0;
    }
}

Type TranslateTypeFromNative(int type) {
    switch (type) {
    case 0:
        return Type::Unspecified;
    case SOCK_STREAM: return Type::STREAM;
    case SOCK_DGRAM: return Type::DGRAM;
    case SOCK_RAW: return Type::RAW;
    case SOCK_SEQPACKET: return Type::SEQPACKET;
    default:
        UNIMPLEMENTED_MSG("Unimplemented type={}", type);
        return Type::STREAM;
    }
}

int TranslateTypeToNative(Type type) {
    switch (type) {
    case Type::Unspecified:
        return 0;
    case Type::STREAM: return SOCK_STREAM;
    case Type::DGRAM: return SOCK_DGRAM;
    case Type::RAW: return SOCK_RAW;
    case Type::SEQPACKET: return SOCK_SEQPACKET;
    default:
        UNIMPLEMENTED_MSG("Unimplemented type={}", type);
        return 0;
    }
}

// Some of those protocols may not be supported on some platforms
// It doesn't really matter, except that some homebrew may not work correctly
// Official software uses TCP & UDP mainly, SCTP is used by some homebrew as well
#ifdef __FreeBSD__
#define NETWORK_PROTOCOL_TRANSLATE_LIST \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ICMP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(TCP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(UDP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IPV6) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(RAW) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IGMP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(GGP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IPV4) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ST) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(EGP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(PIGP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(RCCMON) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(NVPII) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(PUP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ARGUS) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(EMCON) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(XNET) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(CHAOS) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(MUX) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(MEAS) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(HMP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(PRM) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IDP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(TRUNK1) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(TRUNK2) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(LEAF1) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(LEAF2) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(RDP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IRTP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(TP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(BLT) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(NSP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(INP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(DCCP) \
    /*NETWORK_PROTOCOL_TRANSLATE_ELEM(3PC)*/ \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IDPR) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(XTP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(DDP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(CMTP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(TPXX) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IL) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(SDRP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ROUTING) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(FRAGMENT) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IDRP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(RSVP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(GRE) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(MHRP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(BHA) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ESP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(AH) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(INLSP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(SWIPE) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(NHRP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(MOBILE) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(TLSP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(SKIP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ICMPV6) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(NONE) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(DSTOPTS) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(AHIP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(CFTP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(HELLO) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(SATEXPAK) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(KRYPTOLAN) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(RVD) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IPPC) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ADFS) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(SATMON) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(VISA) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IPCV) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(CPNX) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(CPHB) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(WSN) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(PVP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(BRSATMON) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ND) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(WBMON) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(WBEXPAK) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(EON) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(VMTP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(SVMTP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(VINES) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(TTP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IGP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(DGP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(TCF) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IGRP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(OSPFIGP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(SRPC) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(LARP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(MTP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(AX25) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IPEIP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(MICP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(SCCSP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ETHERIP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ENCAP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(APES) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(GMTP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IPCOMP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(SCTP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(MH) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(UDPLITE) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(HIP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(SHIM6) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(PIM) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(CARP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(PGM) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(MPLS) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(PFSYNC)
#elif defined(__linux__)
// Other platforms may not support some niche protocols.
// This is usually not an issue
#define NETWORK_PROTOCOL_TRANSLATE_LIST \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IP) \
    /*NETWORK_PROTOCOL_TRANSLATE_ELEM(HOPOPTS)*/ \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ICMP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IGMP) \
    /*NETWORK_PROTOCOL_TRANSLATE_ELEM(IPIP)*/ \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(TCP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(EGP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(PUP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(UDP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IDP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(TP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(DCCP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IPV6) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ROUTING) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(FRAGMENT) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(RSVP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(GRE) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ESP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(AH) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ICMPV6) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(NONE) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(DSTOPTS) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(MTP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ENCAP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(PIM) \
    /*NETWORK_PROTOCOL_TRANSLATE_ELEM(COMP)*/ \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(SCTP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(UDPLITE)
#elif defined(_WIN32)
#define NETWORK_PROTOCOL_TRANSLATE_LIST \
    /*NETWORK_PROTOCOL_TRANSLATE_ELEM(HOPOPTS)*/ \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ICMP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IGMP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(GGP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IPV4) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ST) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(TCP) \
    /*NETWORK_PROTOCOL_TRANSLATE_ELEM(CBT)*/ \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(EGP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IGP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(PUP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(UDP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IDP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(RDP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(IPV6) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ROUTING) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(FRAGMENT) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ESP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(AH) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ICMPV6) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(NONE) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(DSTOPTS) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(ND) \
    /*NETWORK_PROTOCOL_TRANSLATE_ELEM(ICLFXBM)*/ \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(PIM) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(PGM) \
    /*NETWORK_PROTOCOL_TRANSLATE_ELEM(L2TP)*/ \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(SCTP)
#else
#define NETWORK_PROTOCOL_TRANSLATE_LIST \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(TCP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(UDP) \
    NETWORK_PROTOCOL_TRANSLATE_ELEM(SCTP)
#endif
[[nodiscard]] Protocol TranslateProtocolFromNative(u32 protocol) {
    switch (protocol) {
#define NETWORK_PROTOCOL_TRANSLATE_ELEM(x) case IPPROTO_##x: return Protocol::x;
    NETWORK_PROTOCOL_TRANSLATE_LIST
#undef NETWORK_PROTOCOL_TRANSLATE_ELEM
    default:
        UNIMPLEMENTED_MSG("Unimplemented protocol={}", protocol);
        return Protocol::IP;
    }
}
[[nodiscard]] u32 TranslateProtocolToNative(Protocol protocol) {
    switch (protocol) {
#define NETWORK_PROTOCOL_TRANSLATE_ELEM(x) case Protocol::x: return IPPROTO_##x;
    NETWORK_PROTOCOL_TRANSLATE_LIST
#undef NETWORK_PROTOCOL_TRANSLATE_ELEM
    default:
        UNIMPLEMENTED_MSG("Unimplemented protocol={}", protocol);
        return 0;
    }
}
#undef NETWORK_PROTOCOL_TRANSLATE_LIST

SockAddrIn TranslateToSockAddrIn(sockaddr_in input, size_t input_len) {
    SockAddrIn result{};
    result.family = TranslateDomainFromNative(input.sin_family);
    result.portno = ntohs(input.sin_port);
    result.ip = TranslateIPv4(input.sin_addr);
    return result;
}

short TranslatePollEvents(PollEvents events) {
    short result = 0;

    const auto translate = [&result, &events](PollEvents guest, short host) {
        if (True(events & guest)) {
            events &= ~guest;
            result |= host;
        }
    };

    translate(PollEvents::In, POLLIN);
    translate(PollEvents::Pri, POLLPRI);
    translate(PollEvents::Out, POLLOUT);
    translate(PollEvents::Err, POLLERR);
    translate(PollEvents::Hup, POLLHUP);
    translate(PollEvents::Nval, POLLNVAL);
    translate(PollEvents::RdNorm, POLLRDNORM);
    translate(PollEvents::RdBand, POLLRDBAND);
    translate(PollEvents::WrBand, POLLWRBAND);

#ifdef _WIN32
    // WSAPoll rejects POLLPRI and reports out-of-band data as POLLRDBAND instead; Poll maps it back.
    if ((result & POLLPRI) != 0) {
        result = static_cast<short>((result & ~POLLPRI) | POLLRDBAND);
    }
    short allowed_events = POLLRDBAND | POLLRDNORM | POLLWRNORM;
    // Unlike poll on other OSes, WSAPoll will complain if any other flags are set on input.
    if (result & ~allowed_events) {
        LOG_DEBUG(Network,
                  "Removing WSAPoll input events {:#x} because Windows doesn't support them",
                  result & ~allowed_events);
    }
    result &= allowed_events;
#endif

    UNIMPLEMENTED_IF_MSG((u16)events != 0, "Unhandled guest events={:#x}", (u16)events);

    return result;
}

PollEvents TranslatePollRevents(short revents) {
    PollEvents result{};
    const auto translate = [&result, &revents](short host, PollEvents guest) {
        if ((revents & host) != 0) {
            revents &= static_cast<short>(~host);
            result |= guest;
        }
    };

    translate(POLLIN, PollEvents::In);
    translate(POLLPRI, PollEvents::Pri);
    translate(POLLOUT, PollEvents::Out);
    translate(POLLERR, PollEvents::Err);
    translate(POLLHUP, PollEvents::Hup);
    translate(POLLNVAL, PollEvents::Nval);
    translate(POLLRDNORM, PollEvents::RdNorm);
    translate(POLLRDBAND, PollEvents::RdBand);
    translate(POLLWRBAND, PollEvents::WrBand);

    UNIMPLEMENTED_IF_MSG(revents != 0, "Unhandled host revents={:#x}", revents);

    return result;
}

} // Anonymous namespace

NetworkInstance::NetworkInstance() {
    Initialize();
}

NetworkInstance::~NetworkInstance() {
    Finalize();
}

void CancelPendingSocketOperations() {
    socket_operations_cancelled = true;
    InterruptSocketOperations();
}

void RestartSocketOperations() {
    AcknowledgeInterrupt();
    socket_operations_cancelled = false;
}

bool SocketOperationsCancelled() {
    return socket_operations_cancelled;
}

std::optional<IPv4Address> GetHostIPv4Address() {
    const auto network_interface = Network::GetSelectedNetworkInterface();
    if (!network_interface.has_value()) {
        // Only print the error once to avoid log spam
        static bool print_error = true;
        if (print_error) {
            LOG_ERROR(Network, "GetSelectedNetworkInterface returned no interface");
            print_error = false;
        }

        return {};
    }

    return TranslateIPv4(network_interface->ip_address);
}

std::optional<IPv4Address> GetRoomIPv4Address() {
    if (const auto room_member = GetRoomMember().lock()) {
        // The room host and moderators are in the Moderator state instead of Joined
        const auto state = room_member->GetState();
        if (state == RoomMember::State::Joined || state == RoomMember::State::Moderator) {
            return room_member->GetFakeIpAddress();
        }
    }
    return std::nullopt;
}

std::string IPv4AddressToString(IPv4Address ip_addr) {
    std::array<char, INET_ADDRSTRLEN> buf = {};
    ASSERT(inet_ntop(AF_INET, &ip_addr, buf.data(), sizeof(buf)) == buf.data());
    return std::string(buf.data());
}

u32 IPv4AddressToInteger(IPv4Address ip_addr) {
    return static_cast<u32>(ip_addr[0]) << 24 | static_cast<u32>(ip_addr[1]) << 16 |
           static_cast<u32>(ip_addr[2]) << 8 | static_cast<u32>(ip_addr[3]);
}

std::variant<std::vector<AddrInfo>, GetAddrInfoError> GetAddressInfo(
    const std::string& host, const std::optional<std::string>& service) {
    addrinfo hints{};
    hints.ai_family = AF_INET; // Switch only supports IPv4.
    addrinfo* addrinfo;
    s32 gai_err = getaddrinfo(host.c_str(), service.has_value() ? service->c_str() : nullptr, &hints, &addrinfo);
    if (gai_err != 0) {
        return TranslateGetAddrInfoErrorFromNative(gai_err);
    }
    std::vector<AddrInfo> ret;
    for (auto* current = addrinfo; current; current = current->ai_next) {
        // We should only get AF_INET results due to the hints value.
        ASSERT_OR_EXECUTE(addrinfo->ai_family == AF_INET &&
                              addrinfo->ai_addrlen == sizeof(sockaddr_in),
                          continue;);

        AddrInfo& out = ret.emplace_back();
        out.family = TranslateDomainFromNative(current->ai_family);
        out.socket_type = TranslateTypeFromNative(current->ai_socktype);
        out.protocol = TranslateProtocolFromNative(current->ai_protocol);
        out.addr = TranslateToSockAddrIn(*reinterpret_cast<sockaddr_in*>(current->ai_addr),
                                         current->ai_addrlen);
        if (current->ai_canonname != nullptr) {
            out.canon_name = current->ai_canonname;
        }
    }
    freeaddrinfo(addrinfo);
    return ret;
}

std::pair<s32, Errno> Poll(std::vector<PollFD>& pollfds, s32 timeout) {
    const size_t num = pollfds.size();

    std::vector<WSAPOLLFD> host_pollfds(pollfds.size());
    std::transform(pollfds.begin(), pollfds.end(), host_pollfds.begin(), [](PollFD fd) {
        WSAPOLLFD result;
        result.fd = fd.socket->GetFD();
        result.events = TranslatePollEvents(fd.events);
        result.revents = 0;
        return result;
    });

    host_pollfds.push_back(WSAPOLLFD{
        .fd = GetInterruptSocket(),
        .events = POLLIN,
        .revents = 0,
    });

    const int result =
        WSAPoll(host_pollfds.data(), static_cast<ULONG>(host_pollfds.size()), timeout);
    if (result == 0) {
        ASSERT(std::all_of(host_pollfds.begin(), host_pollfds.end(),
                           [](WSAPOLLFD fd) { return fd.revents == 0; }));
        return {0, Errno::SUCCESS};
    }

    for (size_t i = 0; i < num; ++i) {
        pollfds[i].revents = TranslatePollRevents(host_pollfds[i].revents);
#ifdef _WIN32
        // POLLIN is POLLRDNORM | POLLRDBAND on Windows, which the translation above takes as In
        // alone; normal and out-of-band data stay reported as RdNorm and RdBand too.
        if ((host_pollfds[i].revents & POLLRDNORM) != 0) {
            pollfds[i].revents |= PollEvents::RdNorm;
        }
        if ((host_pollfds[i].revents & POLLRDBAND) != 0) {
            pollfds[i].revents |= PollEvents::RdBand;
            // Out-of-band data asked for as Pri comes back as POLLRDBAND.
            if (True(pollfds[i].events & PollEvents::Pri)) {
                pollfds[i].revents |= PollEvents::Pri;
            }
        }
#endif
    }

    if (result > 0) {
        return {result, Errno::SUCCESS};
    }

    ASSERT(result == SOCKET_ERROR);

    return {-1, GetAndLogLastError()};
}

Socket::~Socket() {
    if (fd == INVALID_SOCKET) {
        return;
    }
    (void)closesocket(fd);
    fd = INVALID_SOCKET;
}

Socket::Socket(Socket&& rhs) noexcept {
    fd = std::exchange(rhs.fd, INVALID_SOCKET);
}

template <typename T>
std::pair<T, Errno> Socket::GetSockOpt(SOCKET fd_so, int level, int option) {
    T value{};
    socklen_t len = sizeof(value);
    const int result = getsockopt(fd_so, level, option, reinterpret_cast<char*>(&value), &len);
    if (result != SOCKET_ERROR) {
        // Windows reports TCP_NODELAY in a single byte. The value starts zeroed, so a shorter
        // option still reads correctly on the little-endian hosts we run on.
        ASSERT(len > 0 && static_cast<size_t>(len) <= sizeof(value));
        return {value, Errno::SUCCESS};
    }
    return {value, GetAndLogLastError()};
}

template <typename T>
Errno Socket::SetSockOpt(SOCKET fd_so, int level, int option, T value) {
    const int result =
        setsockopt(fd_so, level, option, reinterpret_cast<const char*>(&value), sizeof(value));
    if (result != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }
    return GetAndLogLastError();
}

Errno Socket::Initialize(Domain domain, Type type, Protocol protocol) {
    fd = socket(TranslateDomainToNative(domain), TranslateTypeToNative(type), TranslateProtocolToNative(protocol));
    if (fd != INVALID_SOCKET) {
        return Errno::SUCCESS;
    }

    return GetAndLogLastError();
}

std::pair<SocketBase::AcceptResult, Errno> Socket::Accept() {
    sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    const bool wait_for_accept = !is_non_blocking;
    if (wait_for_accept) {
        std::vector<WSAPOLLFD> host_pollfds{
            WSAPOLLFD{fd, POLLIN, 0},
            WSAPOLLFD{GetInterruptSocket(), POLLIN, 0},
        };

        while (true) {
            const int pollres =
                WSAPoll(host_pollfds.data(), static_cast<ULONG>(host_pollfds.size()), -1);
            if (host_pollfds[1].revents != 0) {
                // Interrupt signaled before a client could be accepted, break
                return {AcceptResult{}, Errno::AGAIN};
            }
            if (pollres > 0) {
                break;
            }
        }
    }

    const SOCKET new_socket = accept(fd, reinterpret_cast<sockaddr*>(&addr), &addrlen);

    if (new_socket == INVALID_SOCKET) {
        return {AcceptResult{}, GetAndLogLastError()};
    }

    AcceptResult result{
        .socket = std::make_unique<Socket>(new_socket),
        .sockaddr_in = TranslateToSockAddrIn(addr, addrlen),
    };

    return {std::move(result), Errno::SUCCESS};
}

Errno Socket::Connect(SockAddrIn addr_in) {
    const sockaddr host_addr_in = TranslateFromSockAddrIn(addr_in);
    if (connect(fd, &host_addr_in, sizeof(host_addr_in)) != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }

    return GetAndLogLastError();
}

std::pair<SockAddrIn, Errno> Socket::GetPeerName() {
    sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &addrlen) == SOCKET_ERROR) {
        return {SockAddrIn{}, GetAndLogLastError()};
    }

    return {TranslateToSockAddrIn(addr, addrlen), Errno::SUCCESS};
}

std::pair<SockAddrIn, Errno> Socket::GetSockName() {
    sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &addrlen) == SOCKET_ERROR) {
        return {SockAddrIn{}, GetAndLogLastError()};
    }

    return {TranslateToSockAddrIn(addr, addrlen), Errno::SUCCESS};
}

Errno Socket::Bind(SockAddrIn addr) {
    const sockaddr addr_in = TranslateFromSockAddrIn(addr);
    if (bind(fd, &addr_in, sizeof(addr_in)) != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }

    return GetAndLogLastError();
}

Errno Socket::Listen(s32 backlog) {
    if (listen(fd, backlog) != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }

    return GetAndLogLastError();
}

Errno Socket::Shutdown(ShutdownHow how) {
    int host_how = 0;
    switch (how) {
    case ShutdownHow::RD:
        host_how = SD_RECEIVE;
        break;
    case ShutdownHow::WR:
        host_how = SD_SEND;
        break;
    case ShutdownHow::RDWR:
        host_how = SD_BOTH;
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented flag how={}", how);
        return Errno::SUCCESS;
    }
    if (shutdown(fd, host_how) != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }

    return GetAndLogLastError();
}

template <typename Operation>
std::pair<s32, Errno> Socket::Transfer(int flags, Operation operation) {
#ifdef _WIN32
    if ((flags & FLAG_MSG_DONTWAIT) != 0) {
        if (const Errno begin_errno = BeginDontWait(); begin_errno != Errno::SUCCESS) {
            return {-1, begin_errno};
        }
        const std::pair<s32, Errno> result = operation(0);
        EndDontWait();
        return result;
    }
    while (true) {
        const u64 epoch = dont_wait_epoch;
        const std::pair<s32, Errno> result = operation(0);
        if (result.second != Errno::AGAIN || is_non_blocking || !WaitOutDontWait(epoch)) {
            return result;
        }
    }
#else
    return operation((flags & FLAG_MSG_DONTWAIT) != 0 ? MSG_DONTWAIT : 0);
#endif
}

#ifdef _WIN32
Errno Socket::BeginDontWait() {
    std::scoped_lock lock{mode_mutex};
    if (dont_wait_calls == 0 && !is_non_blocking) {
        if (!EnableNonBlock(fd, true)) {
            return GetAndLogLastError();
        }
        ++dont_wait_epoch;
    }
    ++dont_wait_calls;
    return Errno::SUCCESS;
}

void Socket::EndDontWait() {
    std::scoped_lock lock{mode_mutex};
    if (--dont_wait_calls != 0 || is_non_blocking) {
        return;
    }
    if (!EnableNonBlock(fd, false)) {
        // Blocking calls then fail with EAGAIN as their own, rather than wait for a change.
        GetAndLogLastError();
    }
    ++dont_wait_epoch;
    dont_wait_ended.notify_all();
}

bool Socket::WaitOutDontWait(u64 epoch) {
    std::unique_lock lock{mode_mutex};
    // Unless calls given FLAG_MSG_DONTWAIT made the host socket nonblocking while the call ran,
    // its EAGAIN is its own, as it is once the socket is nonblocking.
    if (is_non_blocking || (epoch % 2 == 0 && dont_wait_epoch == epoch)) {
        return false;
    }
    dont_wait_ended.wait(lock, [this] { return dont_wait_epoch % 2 == 0 || is_non_blocking; });
    return !is_non_blocking;
}
#endif

std::pair<s32, Errno> Socket::Recv(int flags, std::span<u8> message) {
    ASSERT((flags & ~FLAG_MSG_DONTWAIT) == 0);
    ASSERT(message.size() < static_cast<size_t>((std::numeric_limits<int>::max)()));

    return Transfer(flags, [&](int host_flags) -> std::pair<s32, Errno> {
        const auto result = recv(fd, reinterpret_cast<char*>(message.data()),
                                 static_cast<int>(message.size()), host_flags);
        if (result != SOCKET_ERROR) {
            return {static_cast<s32>(result), Errno::SUCCESS};
        }

        return {-1, GetAndLogLastError()};
    });
}

std::pair<s32, Errno> Socket::RecvFrom(int flags, std::span<u8> message, SockAddrIn* addr) {
    ASSERT((flags & ~FLAG_MSG_DONTWAIT) == 0);
    ASSERT(message.size() < static_cast<size_t>((std::numeric_limits<int>::max)()));

    return Transfer(flags, [&](int host_flags) -> std::pair<s32, Errno> {
        sockaddr_in addr_in{};
        socklen_t addrlen = sizeof(addr_in);
        socklen_t* const p_addrlen = addr ? &addrlen : nullptr;
        sockaddr* const p_addr_in = addr ? reinterpret_cast<sockaddr*>(&addr_in) : nullptr;

        const auto result =
            recvfrom(fd, reinterpret_cast<char*>(message.data()),
                     static_cast<int>(message.size()), host_flags, p_addr_in, p_addrlen);
        if (result != SOCKET_ERROR) {
            if (addr) {
                *addr = TranslateToSockAddrIn(addr_in, addrlen);
            }
            return {static_cast<s32>(result), Errno::SUCCESS};
        }

        return {-1, GetAndLogLastError()};
    });
}

std::pair<s32, Errno> Socket::RecvMsg(int flags, std::span<u8> message, SockAddrIn* addr,
                                      u32& msg_flags) {
    ASSERT((flags & ~(FLAG_MSG_PEEK | FLAG_MSG_DONTWAIT)) == 0);
    ASSERT(message.size() < static_cast<size_t>((std::numeric_limits<int>::max)()));

    msg_flags = 0;
    const int peek_flags = (flags & FLAG_MSG_PEEK) != 0 ? MSG_PEEK : 0;
    return Transfer(flags, [&](int host_flags) -> std::pair<s32, Errno> {
        const int native_flags = peek_flags | host_flags;
        sockaddr_in addr_in{};
#ifdef _WIN32
        // A datagram that does not fit fills message with its start and fails with WSAEMSGSIZE,
        // which recvmsg reports as a successful receive with MSG_TRUNC.
        socklen_t addrlen = sizeof(addr_in);
        const auto result =
            recvfrom(fd, reinterpret_cast<char*>(message.data()), static_cast<int>(message.size()),
                     native_flags, addr ? reinterpret_cast<sockaddr*>(&addr_in) : nullptr,
                     addr ? &addrlen : nullptr);
        s32 received = static_cast<s32>(result);
        if (result == SOCKET_ERROR) {
            if (WSAGetLastError() != WSAEMSGSIZE) {
                return {-1, GetAndLogLastError()};
            }
            received = static_cast<s32>(message.size());
            msg_flags |= FLAG_MSG_TRUNC;
        }
#else
        iovec iov{};
        iov.iov_base = message.data();
        iov.iov_len = message.size();
        msghdr header{};
        header.msg_name = addr ? &addr_in : nullptr;
        header.msg_namelen = addr ? static_cast<socklen_t>(sizeof(addr_in)) : 0;
        header.msg_iov = &iov;
        header.msg_iovlen = 1;
        const auto result = recvmsg(fd, &header, native_flags);
        if (result == SOCKET_ERROR) {
            return {-1, GetAndLogLastError()};
        }
        const socklen_t addrlen = header.msg_namelen;
        const s32 received = static_cast<s32>(result);
        if ((header.msg_flags & MSG_TRUNC) != 0) {
            msg_flags |= FLAG_MSG_TRUNC;
        }
#endif
        if (addr) {
            *addr = TranslateToSockAddrIn(addr_in, addrlen);
        }
        return {received, Errno::SUCCESS};
    });
}

std::pair<s32, Errno> Socket::Send(std::span<const u8> message, int flags) {
    ASSERT(message.size() < static_cast<size_t>((std::numeric_limits<int>::max)()));
    ASSERT((flags & ~FLAG_MSG_DONTWAIT) == 0);

    int native_flags = 0;
#ifdef __unix__
    native_flags |= MSG_NOSIGNAL; // do not send us SIGPIPE
#endif
    return Transfer(flags, [&](int host_flags) -> std::pair<s32, Errno> {
        const auto result = send(fd, reinterpret_cast<const char*>(message.data()),
                                 static_cast<int>(message.size()), native_flags | host_flags);
        if (result != SOCKET_ERROR) {
            return {static_cast<s32>(result), Errno::SUCCESS};
        }

        return {-1, GetAndLogLastError(CallType::Send)};
    });
}

std::pair<s32, Errno> Socket::SendTo(u32 flags, std::span<const u8> message,
                                     const SockAddrIn* addr) {
    ASSERT((flags & ~FLAG_MSG_DONTWAIT) == 0);

    const sockaddr* to = nullptr;
    const int to_len = addr ? sizeof(sockaddr) : 0;
    sockaddr host_addr_in;

    if (addr) {
        host_addr_in = TranslateFromSockAddrIn(*addr);
        to = &host_addr_in;
    }

    return Transfer(static_cast<int>(flags), [&](int host_flags) -> std::pair<s32, Errno> {
        const auto result = sendto(fd, reinterpret_cast<const char*>(message.data()),
                                   static_cast<int>(message.size()), host_flags, to, to_len);
        if (result != SOCKET_ERROR) {
            return {static_cast<s32>(result), Errno::SUCCESS};
        }

        return {-1, GetAndLogLastError(CallType::Send)};
    });
}

Errno Socket::Close() {
    [[maybe_unused]] const int result = closesocket(fd);
    if (result != 0) {
        LOG_WARNING(Network, "closesocket failed, socket may already be closed");
    }
    fd = INVALID_SOCKET;

    return Errno::SUCCESS;
}

std::pair<Errno, Errno> Socket::GetPendingError() {
    auto [pending_err, getsockopt_err] = GetSockOpt<int>(fd, SOL_SOCKET, SO_ERROR);
    return {TranslateNativeError(pending_err), getsockopt_err};
}

std::pair<bool, Errno> Socket::GetReuseAddr() {
    const auto [value, getsockopt_err] = GetSockOpt<u32>(fd, SOL_SOCKET, SO_REUSEADDR);
    return {value != 0, getsockopt_err};
}

std::pair<bool, Errno> Socket::GetNoDelay() {
    const auto [value, getsockopt_err] = GetSockOpt<u32>(fd, IPPROTO_TCP, TCP_NODELAY);
    return {value != 0, getsockopt_err};
}

Errno Socket::SetLinger(bool enable, u32 linger) {
    return SetSockOpt(fd, SOL_SOCKET, SO_LINGER, MakeLinger(enable, linger));
}

Errno Socket::SetReuseAddr(bool enable) {
    return SetSockOpt<u32>(fd, SOL_SOCKET, SO_REUSEADDR, enable ? 1 : 0);
}

Errno Socket::SetKeepAlive(bool enable) {
    return SetSockOpt<u32>(fd, SOL_SOCKET, SO_KEEPALIVE, enable ? 1 : 0);
}

Errno Socket::SetBroadcast(bool enable) {
    return SetSockOpt<u32>(fd, SOL_SOCKET, SO_BROADCAST, enable ? 1 : 0);
}

Errno Socket::SetSndBuf(u32 value) {
    return SetSockOpt(fd, SOL_SOCKET, SO_SNDBUF, value);
}

Errno Socket::SetRcvBuf(u32 value) {
    return SetSockOpt(fd, SOL_SOCKET, SO_RCVBUF, value);
}

Errno Socket::SetSndTimeo(u32 value) {
    return SetSockOpt(fd, SOL_SOCKET, SO_SNDTIMEO, MakeTimeout(value));
}

Errno Socket::SetRcvTimeo(u32 value) {
    return SetSockOpt(fd, SOL_SOCKET, SO_RCVTIMEO, MakeTimeout(value));
}

Errno Socket::SetNoDelay(bool enable) {
    return SetSockOpt<u32>(fd, IPPROTO_TCP, TCP_NODELAY, enable ? 1 : 0);
}

Errno Socket::SetNonBlock(bool enable) {
#ifdef _WIN32
    std::scoped_lock lock{mode_mutex};
    if (dont_wait_calls > 0) {
        // The host socket stays nonblocking for the calls given FLAG_MSG_DONTWAIT, and the last
        // of them leaves it in this mode. Whether it is nonblocking for them alone changes.
        if (enable != is_non_blocking) {
            is_non_blocking = enable;
            ++dont_wait_epoch;
            dont_wait_ended.notify_all();
        }
        return Errno::SUCCESS;
    }
#endif
    if (EnableNonBlock(fd, enable)) {
        is_non_blocking = enable;
        return Errno::SUCCESS;
    }
    return GetAndLogLastError();
}

bool Socket::IsOpened() const {
    return fd != INVALID_SOCKET;
}

bool Socket::AcceptsProxyPacket(const ProxyPacket& packet) {
    return false;
}

void Socket::HandleProxyPacket(const ProxyPacket& packet) {
    LOG_WARNING(Network, "ProxyPacket received, but not in Proxy mode!");
}

std::optional<PollEvents> Socket::EmulatedPoll(PollEvents events) {
    return std::nullopt;
}

} // namespace Network
