// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging.h"

#include "core/hle/service/ssl/ssl_backend.h"

namespace Service::SSL {

Result CreateSSLConnectionBackend(std::unique_ptr<SSLConnectionBackend>* out_backend) {
    LOG_ERROR(Service_SSL,
              "Can't create SSL connection because no SSL backend is available on this platform");
    return ResultInternalError;
}

Result CreateSSLTrustStore(const SSLTrustStore* base,
                           std::span<const std::span<const u8>> der_certificates,
                           std::shared_ptr<const SSLTrustStore>* out_store) {
    LOG_ERROR(Service_SSL,
              "Can't create SSL trust store because no SSL backend is available on this platform");
    return ResultInternalError;
}

Result DecodeServerPki(std::span<const u8> data, std::vector<std::vector<u8>>* out_certificates) {
    LOG_ERROR(Service_SSL,
              "Can't decode server PKI because no SSL backend is available on this platform");
    return ResultInternalError;
}

} // namespace Service::SSL
