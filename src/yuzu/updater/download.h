// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <string>
#include "common/common_types.h"

class QSaveFile;

namespace UpdateDownload {

struct Request {
    // Scheme and host ("https://github.com") and the path on it, as httplib wants them.
    std::string url;
    std::string path;
    // Lowercase hex SHA-256 and byte size to check; empty and zero skip the check.
    std::string sha256;
    u64 size{};
};

struct Callbacks {
    // Called with bytes received and the total (0 when unknown); false cancels.
    std::function<bool(u64 processed, u64 total)> progress;
    // Called before every retry, with the file already started over, to wait and show it; false
    // cancels.
    std::function<bool(int attempt, int attempts)> retry;
};

enum class Status { Ok, Canceled, WriteFailed, Network, HttpError, BadChecksum };

struct Outcome {
    Status status{Status::Ok};
    int http_status{};
    std::string error;
};

// Download into `file`, which must be open for writing, retrying a few times on a dropped
// connection or a 429/5xx answer. Everything is verified against `request`; the caller commits the
// file on Ok and cancels it otherwise. Touches no widgets, so it may run on a worker thread.
Outcome Download(const Request& request, QSaveFile& file, const Callbacks& callbacks);

} // namespace UpdateDownload
