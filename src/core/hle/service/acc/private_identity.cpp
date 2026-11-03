// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <fmt/format.h>

#include "core/hle/service/acc/private_identity.h"

namespace Service::Account::PrivateServer {

namespace {

/// Encodes data as unpadded base64url (RFC 4648 section 5), as JWT segments are.
std::string Base64UrlEncode(std::string_view data) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string out;
    out.reserve((data.size() * 4 + 2) / 3);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::size_t remaining = data.size() - i;
        u32 group = u32{static_cast<u8>(data[i])} << 16;
        if (remaining > 1) {
            group |= u32{static_cast<u8>(data[i + 1])} << 8;
        }
        if (remaining > 2) {
            group |= u32{static_cast<u8>(data[i + 2])};
        }
        out += alphabet[(group >> 18) & 0x3F];
        out += alphabet[(group >> 12) & 0x3F];
        if (remaining > 1) {
            out += alphabet[(group >> 6) & 0x3F];
        }
        if (remaining > 2) {
            out += alphabet[group & 0x3F];
        }
    }
    return out;
}

} // Anonymous namespace

u64 GetAccountId(const Common::UUID& user_id) {
    // The fixed bit keeps the id from being zero.
    return static_cast<u64>(user_id.Hash()) | 0x1000000000000000ULL;
}

std::string MakeIdToken(u64 account_id, u64 application_id, s64 issued_at) {
    const std::string claims =
        fmt::format(R"({{"iss":"{}","sub":"{:016x}","aud":"{:016X}","iat":{},"exp":{}}})",
                    IdTokenIssuer, account_id, application_id, issued_at,
                    issued_at + IdTokenLifetimeSeconds);
    // The header is {"alg":"none","typ":"JWT"}, and the signature is empty. Tools that look for
    // this feature in the binary search for the encoded header, so it stays a single literal.
    return fmt::format("eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0.{}.", Base64UrlEncode(claims));
}

} // namespace Service::Account::PrivateServer
