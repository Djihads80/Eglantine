// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <span>
#include <vector>

#include "core/file_sys/vfs/vfs_types.h"
#include "core/hle/result.h"
#include "core/hle/service/ssl/ssl_types.h"

namespace Core {
class System;
}

namespace Service::SSL {

class SSLTrustStore;

class CertStore {
public:
    /// The id of the private server CA among the built-in certificates, which no firmware uses
    static constexpr auto PrivateServerCaId = static_cast<CaCertificateId>(0x7F000001);

    /// Loads the built-in certificates of the system's CertStore archive, see the other constructor
    explicit CertStore(Core::System& system);
    /// Loads the built-in certificates of ssl_TrustedCerts.bdf, which is null without firmware.
    /// With Settings::values.private_server_enabled, the private_server_ca certificate is one more,
    /// with status EnabledTrusted, which every request of specific ids also returns.
    explicit CertStore(FileSys::VirtualFile built_in_certificates);
    ~CertStore();

    Result GetCertificates(u32* out_num_entries, std::span<u8> out_data,
                           std::span<const CaCertificateId> certificate_ids);
    Result GetCertificateBufSize(u32* out_size, u32* out_num_entries,
                                 std::span<const CaCertificateId> certificate_ids);

    /// A store trusting the certificates with status EnabledTrusted, which every ISslContext
    /// trusts as on the console. Null if the SSL backend cannot make one.
    std::shared_ptr<const SSLTrustStore> GetTrustStore();

private:
    void LoadBuiltInCertificates(const FileSys::VfsFile& cert_store_file);

    template <typename F>
    void ForEachCertificate(std::span<const CaCertificateId> certs, F&& f);

private:
    struct Certificate {
        TrustedCertStatus status;
        std::vector<u8> der_data;
    };

    std::map<CaCertificateId, Certificate> m_certs;
    std::shared_ptr<const SSLTrustStore> m_trust_store;
    /// Whether m_certs holds the private server CA, as PrivateServerCaId
    bool m_has_private_server_ca = false;
};

} // namespace Service::SSL
