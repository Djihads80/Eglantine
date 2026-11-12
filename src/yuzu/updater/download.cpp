// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QSaveFile>
#include "common/logging.h"
#include "download.h"

#include "common/httplib.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace UpdateDownload {

Outcome Download(const Request& request, QSaveFile& file, const Callbacks& callbacks) {
    const std::string filename = file.fileName().toStdString();

    // TODO(crueter): Move to net.cpp
    constexpr std::size_t timeout_seconds = 15;

    httplib::Client client(request.url);
    client.set_connection_timeout(timeout_seconds);
    client.set_read_timeout(timeout_seconds);
    client.set_write_timeout(timeout_seconds);
    // GitHub redirects release downloads to another host.
    client.set_follow_location(true);

#ifdef YUZU_BUNDLED_OPENSSL
    client.load_ca_cert_store(kCert, sizeof(kCert));
#endif

    bool canceled = false;
    auto progress_callback = [&](size_t processed_size, size_t total_size) {
        if (callbacks.progress && !callbacks.progress(processed_size, total_size)) {
            canceled = true;
        }
        return !canceled;
    };

    QCryptographicHash hash(QCryptographicHash::Sha256);
    u64 received = 0;
    bool body_wanted = false;
    bool write_failed = false;

    // A redirect response carries no file, only the final answer is saved.
    auto response_handler = [&body_wanted](const httplib::Response& response) {
        body_wanted = response.status >= 200 && response.status < 300;
        return true;
    };

    auto content_receiver = [&](const char* t_data, size_t data_length) -> bool {
        if (!body_wanted) {
            return true;
        }
        hash.addData(QByteArrayView(t_data, static_cast<qsizetype>(data_length)));
        received += data_length;
        if (file.write(t_data, data_length) == -1) {
            LOG_WARNING(Frontend, "Could not write {} bytes to file {}", data_length, filename);
            write_failed = true;
            return false;
        }

        return true;
    };

    // GitHub's file server sometimes answers 502-504 or drops the connection for a moment: try up
    // to three times, a few seconds apart, starting the file over each time.
    constexpr int attempts = 3;
    httplib::Result result;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (attempt > 1) {
            file.seek(0);
            file.resize(0);
            hash.reset();
            received = 0;
            if (callbacks.retry && !callbacks.retry(attempt, attempts)) {
                canceled = true;
                break;
            }
        }
        body_wanted = false;
        result = client.Get(request.path, response_handler, content_receiver, progress_callback);
        const bool transient = !write_failed && !canceled &&
                               ((!result && result.error() != httplib::Error::Canceled) ||
                                (result && (result->status >= 500 || result->status == 429)));
        if (!transient || attempt == attempts) {
            break;
        }
        LOG_WARNING(Frontend, "GET to {}{} failed ({}), trying again", request.url, request.path,
                    result ? std::to_string(result->status) : httplib::to_string(result.error()));
    }

    if (canceled) {
        return {Status::Canceled, 0, {}};
    }
    if (write_failed) {
        return {Status::WriteFailed, 0, {}};
    }
    if (!result) {
        LOG_ERROR(Frontend, "GET to {}{} failed: {}", request.url, request.path,
                  httplib::to_string(result.error()));
        return {Status::Network, 0, httplib::to_string(result.error())};
    }

    const auto& response = result.value();
    if (response.status < 200 || response.status >= 300) {
        LOG_ERROR(Frontend, "GET to {}{} returned error status code: {}", request.url,
                  request.path, response.status);
        return {Status::HttpError, response.status, {}};
    }

    const bool wrong_size = request.size != 0 && received != request.size;
    const bool wrong_hash = !request.sha256.empty() &&
                            hash.result().toHex() != QByteArray::fromStdString(request.sha256);
    if (wrong_size || wrong_hash) {
        LOG_ERROR(Frontend, "Downloaded {}{} does not match the published size or SHA-256",
                  request.url, request.path);
        return {Status::BadChecksum, 0, {}};
    }
    return {};
}

} // namespace UpdateDownload
