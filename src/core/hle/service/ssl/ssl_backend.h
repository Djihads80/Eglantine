// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "common/common_funcs.h"
#include "common/common_types.h"

#include "core/hle/result.h"

namespace Network {
class SocketBase;
}

namespace Service::SSL {

constexpr Result ResultNoSocket{ErrorModule::SSLSrv, 103};
constexpr Result ResultInvalidSocket{ErrorModule::SSLSrv, 106};
constexpr Result ResultTimeout{ErrorModule::SSLSrv, 205};
constexpr Result ResultMaxServerPkiRegistered{ErrorModule::SSLSrv, 218};
constexpr Result ResultSslErrorInvalidCertDomain{ErrorModule::SSLSrv, 301};
constexpr Result ResultSslErrorExpiredCertificate{ErrorModule::SSLSrv, 303};
constexpr Result ResultSslErrorRevokedCertificate{ErrorModule::SSLSrv, 304};
constexpr Result ResultSslErrorBadCertificate{ErrorModule::SSLSrv, 307};
constexpr Result ResultSslErrorUnknownCa{ErrorModule::SSLSrv, 308};
constexpr Result ResultInternalError{ErrorModule::SSLSrv, 999}; // made up

// ResultWouldBlock is returned from Read and Write, and oddly, DoHandshake,
// with no way in the latter case to distinguish whether the client should poll
// for read or write.  The one official client I've seen handles this by always
// polling for read (with a timeout).
constexpr Result ResultWouldBlock{ErrorModule::SSLSrv, 204};

// This is nn::ssl::sf::VerifyOption. A connection starts with PeerCa | HostName.
enum class VerifyOption : u32 {
    None = 0,
    PeerCa = 1 << 0,            ///< The peer's chain ends at a trusted certificate
    HostName = 1 << 1,          ///< The peer's certificate is for the host name set
    DateCheck = 1 << 2,         ///< The chain's certificates are valid now
    EvCertPartial = 1 << 3,     ///< Extended validation, not implemented
    EvPolicyOid = 1 << 4,       ///< Extended validation, not implemented
    EvCertFingerprint = 1 << 5, ///< Extended validation, not implemented
};
DECLARE_ENUM_FLAG_OPERATORS(VerifyOption)

/// The certificates a verified peer chains to: those of the console's built-in certificates it
/// trusts and the server PKIs imported into the context. Each of them may end a chain, as a server
/// certificate imported to be trusted directly does. Defined by the backend.
class SSLTrustStore;

/// Creates a store trusting the certificates base trusts, if not null, and der_certificates.
Result CreateSSLTrustStore(const SSLTrustStore* base,
                           std::span<const std::span<const u8>> der_certificates,
                           std::shared_ptr<const SSLTrustStore>* out_store);

/// Decodes the certificates of an ImportServerPki buffer, PEM or DER, into DER. Fails if one is
/// malformed or there are none.
Result DecodeServerPki(std::span<const u8> data, std::vector<std::vector<u8>>* out_certificates);

class SSLConnectionBackend {
public:
    virtual ~SSLConnectionBackend() {}
    virtual void SetSocket(std::shared_ptr<Network::SocketBase> socket) = 0;
    /// Takes effect when the handshake starts, as does the trust store
    virtual void SetVerifyOption(VerifyOption option) = 0;
    /// The certificates VerifyOption::PeerCa trusts. Without a store, none are.
    virtual void SetTrustStore(std::shared_ptr<const SSLTrustStore> store) = 0;
    virtual Result DoHandshake() = 0;
    /// The failure of the peer's verification, then success until it fails again
    virtual Result GetVerifyCertError() = 0;
    virtual Result Read(size_t* out_size, std::span<u8> data) = 0;
    virtual Result Peek(size_t* out_size, std::span<u8> data) = 0;
    virtual Result Write(size_t* out_size, std::span<const u8> data) = 0;
    virtual Result GetServerCerts(std::vector<std::vector<u8>>* out_certs) = 0;
    virtual Result SetHostName(const char* hostname) = 0;
    virtual Result GetHostName(std::span<u8> hostname, u32* out_size) = 0;
    virtual int Pending() = 0;
    virtual Result SetRenegotiationMode(u32 mode) = 0;
    virtual Result GetRenegotiationMode(u32* mode) = 0;
};

Result CreateSSLConnectionBackend(std::unique_ptr<SSLConnectionBackend>* out_backend);

} // namespace Service::SSL
