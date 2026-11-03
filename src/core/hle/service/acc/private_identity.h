// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

#include "common/common_types.h"
#include "common/uuid.h"

// The account identity an explicitly configured private server sees. It is made locally from the
// emulator profile, proves nothing about the user and is never a Nintendo credential.
namespace Service::Account::PrivateServer {

/// Issuer of the id token. No identity provider can own a .invalid domain (RFC 6761).
constexpr std::string_view IdTokenIssuer = "https://private-server.invalid";

/// How long an id token stays valid. Every load makes a new token, so this only limits how long
/// the guest may keep using one it has already loaded.
constexpr s64 IdTokenLifetimeSeconds = 24 * 60 * 60;

/// Returns the account id of a local profile: a function of its UUID alone, so it is the same in
/// every session, and never zero. Changing it changes the identity servers have recorded.
u64 GetAccountId(const Common::UUID& user_id);

/// Makes the id token of an account for an application, issued at a Unix time in seconds. It is
/// an unsigned JWT ("alg":"none") whose subject is the account id in 16 lowercase hex digits and
/// whose audience is the application id in 16 uppercase hex digits.
std::string MakeIdToken(u64 account_id, u64 application_id, s64 issued_at);

} // namespace Service::Account::PrivateServer
