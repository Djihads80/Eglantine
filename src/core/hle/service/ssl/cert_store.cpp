// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>

#include "common/alignment.h"
#include "common/fs/file.h"
#include "common/fs/fs_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/ssl/cert_store.h"
#include "core/hle/service/ssl/ssl_backend.h"

namespace Service::SSL {

// https://switchbrew.org/wiki/SSL_services#CertStore

namespace {

/// ssl_TrustedCerts.bdf of the system's CertStore archive, or null if the firmware lacks it
FileSys::VirtualFile FindBuiltInCertificates(Core::System& system) {
    constexpr u64 CertStoreDataId = 0x0100000000000800ULL;

    auto& fsc = system.GetFileSystemController();

    // Attempt to load certificate data from storage
    const auto nca =
        fsc.GetSystemNANDContents()->GetEntry(CertStoreDataId, FileSys::ContentRecordType::Data);
    if (!nca) {
        return nullptr;
    }
    const auto romfs = nca->GetRomFS();
    if (!romfs) {
        return nullptr;
    }
    const auto extracted = FileSys::ExtractRomFS(romfs);
    if (!extracted) {
        LOG_ERROR(Service_SSL, "CertStore could not be extracted, corrupt RomFS?");
        return nullptr;
    }
    const auto cert_store_file = extracted->GetFile("ssl_TrustedCerts.bdf");
    if (!cert_store_file) {
        LOG_ERROR(Service_SSL, "Failed to find trusted certificates in CertStore");
        return nullptr;
    }
    return cert_store_file;
}

/// The certificate of the private_server_ca file, if the private server is enabled. Nothing if it
/// is not, or the file does not hold exactly one DER certificate: a bad configuration adds no
/// trust.
std::optional<std::vector<u8>> ReadPrivateServerCa() {
    if (!Settings::values.private_server_enabled.GetValue()) {
        return std::nullopt;
    }
    const std::string path = Settings::values.private_server_ca.GetValue();
    if (path.empty()) {
        LOG_INFO(Service_SSL, "No private server CA is configured: private servers have to present "
                              "certificates the built-in certificates trust");
        return std::nullopt;
    }
    const std::string file = Common::FS::ReadStringFromFile(
        std::filesystem::path{Common::FS::ToU8String(path)}, Common::FS::FileType::BinaryFile);
    if (file.empty()) {
        LOG_ERROR(Service_SSL, "Not trusting the private server CA, as {} could not be read", path);
        return std::nullopt;
    }
    std::vector<u8> der(file.begin(), file.end());
    // DecodeServerPki also takes PEM and several certificates, whose decoding is not the file.
    std::vector<std::vector<u8>> certificates;
    if (DecodeServerPki(der, &certificates).IsError() || certificates.size() != 1 ||
        certificates.front() != der) {
        LOG_ERROR(Service_SSL,
                  "Not trusting the private server CA, as {} is not exactly one DER certificate",
                  path);
        return std::nullopt;
    }
    return der;
}

} // namespace

CertStore::CertStore(Core::System& system) : CertStore{FindBuiltInCertificates(system)} {}

CertStore::CertStore(FileSys::VirtualFile built_in_certificates) {
    if (built_in_certificates) {
        LoadBuiltInCertificates(*built_in_certificates);
    }

    // A built-in certificate the console trusts, so that both the system's SSL and titles building
    // their own trust store from GetCertificates accept the private server's chain.
    if (auto ca = ReadPrivateServerCa()) {
        if (m_certs.contains(PrivateServerCaId)) {
            LOG_ERROR(Service_SSL,
                      "Not trusting the private server CA, as the firmware uses its id {:#x}",
                      static_cast<u32>(PrivateServerCaId));
        } else {
            LOG_INFO(Service_SSL,
                     "Trusting the private server CA {} ({} bytes) as certificate id {:#x}",
                     Settings::values.private_server_ca.GetValue(), ca->size(),
                     static_cast<u32>(PrivateServerCaId));
            m_certs.emplace(PrivateServerCaId, Certificate{
                                                   .status = TrustedCertStatus::EnabledTrusted,
                                                   .der_data = std::move(*ca),
                                               });
            m_has_private_server_ca = true;
        }
    }
}

void CertStore::LoadBuiltInCertificates(const FileSys::VfsFile& cert_store_file) {
    // Read and verify the header.
    CertStoreHeader header;
    cert_store_file.ReadObject(std::addressof(header));

    if (header.magic != Common::MakeMagic('s', 's', 'l', 'T')) {
        LOG_ERROR(Service_SSL, "Invalid certificate store magic");
        return;
    }

    // Ensure the file can contains the number of entries it says it does.
    const u64 expected_size = sizeof(header) + sizeof(CertStoreEntry) * header.num_entries;
    const u64 actual_size = cert_store_file.GetSize();
    if (actual_size < expected_size) {
        LOG_ERROR(Service_SSL, "Size mismatch, expected at least {} bytes, got {}", expected_size,
                  actual_size);
        return;
    }

    // Read entries.
    std::vector<CertStoreEntry> entries(header.num_entries);
    cert_store_file.ReadArray(entries.data(), header.num_entries, sizeof(header));

    // Insert into memory store.
    for (const auto& entry : entries) {
        m_certs.emplace(entry.certificate_id,
                        Certificate{
                            .status = entry.certificate_status,
                            .der_data = cert_store_file.ReadBytes(
                                entry.der_size, entry.der_offset + sizeof(header)),
                        });
    }
}

CertStore::~CertStore() = default;

template <typename F>
void CertStore::ForEachCertificate(std::span<const CaCertificateId> certificate_ids, F&& f) {
    if (certificate_ids.size() == 1 && certificate_ids.front() == CaCertificateId::All) {
        for (const auto& entry : m_certs) {
            f(entry);
        }
    } else {
        for (const auto certificate_id : certificate_ids) {
            const auto entry = m_certs.find(certificate_id);
            if (entry == m_certs.end()) {
                continue;
            }
            f(*entry);
        }

        // Titles request known console IDs rather than the private CA's ID. Unknown-only queries
        // must remain empty instead of unexpectedly returning an unrelated certificate.
        const bool has_known_id = std::ranges::any_of(certificate_ids, [this](CaCertificateId id) {
            const auto value = static_cast<s32>(id);
            return (value >= static_cast<s32>(CaCertificateId::NintendoCAG3) &&
                    value <= static_cast<s32>(CaCertificateId::NintendoRootCAG4)) ||
                   (value >= static_cast<s32>(CaCertificateId::AmazonRootCA1) &&
                    value <= static_cast<s32>(CaCertificateId::DigiCertTLSRSA4096RootG5)) ||
                   id == CaCertificateId::NintendoTempRootCAG4 || m_certs.contains(id);
        });
        if (m_has_private_server_ca && has_known_id &&
            std::ranges::find(certificate_ids, PrivateServerCaId) == certificate_ids.end()) {
            f(*m_certs.find(PrivateServerCaId));
        }
    }
}

Result CertStore::GetCertificates(u32* out_num_entries, std::span<u8> out_data,
                                  std::span<const CaCertificateId> certificate_ids) {
    // Ensure the buffer is large enough to hold the output.
    u32 required_size;
    R_TRY(this->GetCertificateBufSize(std::addressof(required_size), out_num_entries,
                                      certificate_ids));
    R_UNLESS(out_data.size_bytes() >= required_size, ResultUnknown);

    // Make parallel arrays.
    std::vector<BuiltInCertificateInfo> cert_infos;
    std::vector<u8> der_datas;

    const u32 der_data_offset = (*out_num_entries + 1) * sizeof(BuiltInCertificateInfo);
    u32 cur_der_offset = der_data_offset;

    // Fill output.
    this->ForEachCertificate(certificate_ids, [&](auto& entry) {
        const auto& [status, cur_der_data] = entry.second;
        BuiltInCertificateInfo cert_info{
            .cert_id = entry.first,
            .status = status,
            .der_size = cur_der_data.size(),
            .der_offset = cur_der_offset,
        };

        cert_infos.push_back(cert_info);
        der_datas.insert(der_datas.end(), cur_der_data.begin(), cur_der_data.end());
        cur_der_offset += static_cast<u32>(cur_der_data.size());
    });

    // Append terminator entry.
    cert_infos.push_back(BuiltInCertificateInfo{
        .cert_id = CaCertificateId::All,
        .status = TrustedCertStatus::Invalid,
        .der_size = 0,
        .der_offset = 0,
    });

    // Write to output span.
    std::memcpy(out_data.data(), cert_infos.data(),
                cert_infos.size() * sizeof(BuiltInCertificateInfo));
    std::memcpy(out_data.data() + der_data_offset, der_datas.data(), der_datas.size());

    R_SUCCEED();
}

Result CertStore::GetCertificateBufSize(u32* out_size, u32* out_num_entries,
                                        std::span<const CaCertificateId> certificate_ids) {
    // Output size is at least the size of the terminator entry.
    *out_size = sizeof(BuiltInCertificateInfo);
    *out_num_entries = 0;

    this->ForEachCertificate(certificate_ids, [&](auto& entry) {
        *out_size += sizeof(BuiltInCertificateInfo);
        *out_size += Common::AlignUp(static_cast<u32>(entry.second.der_data.size()), 4);
        (*out_num_entries)++;
    });

    R_SUCCEED();
}

std::shared_ptr<const SSLTrustStore> CertStore::GetTrustStore() {
    if (m_trust_store) {
        return m_trust_store;
    }
    std::vector<std::span<const u8>> trusted;
    for (const auto& [id, certificate] : m_certs) {
        if (certificate.status == TrustedCertStatus::EnabledTrusted) {
            trusted.emplace_back(certificate.der_data);
        }
    }
    if (trusted.empty()) {
        LOG_WARNING(Service_SSL, "No trusted built-in certificates, as the system firmware is "
                                 "missing: connections verifying their peer will fail");
    }
    if (CreateSSLTrustStore(nullptr, trusted, &m_trust_store).IsError()) {
        m_trust_store.reset();
    }
    return m_trust_store;
}

} // namespace Service::SSL
