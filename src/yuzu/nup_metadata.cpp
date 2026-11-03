// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/nup_metadata.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <set>
#include <unordered_map>

#include <boost/algorithm/string/replace.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QFileInfo>
#include <QImageReader>
#include <QtConcurrent/QtConcurrentRun>

#include "common/fs/file.h"
#include "common/fs/fs_util.h"
#include "common/httplib.h"
#include "common/logging.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/nup_writer.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace NupMetadata {

namespace fs = std::filesystem;

namespace {

constexpr int TargetIconSize = 256;
constexpr int MaxDecodeDimension = 8192;
constexpr int HttpTimeoutSeconds = 2;
constexpr auto OverallTimeout = std::chrono::seconds(5);

std::string NormalizeSlug(std::string title) {
    title = Common::ToLower(title);
    boost::replace_all(title, "é", "e");
    boost::replace_all(title, "è", "e");
    boost::replace_all(title, "ê", "e");
    boost::replace_all(title, "ō", "o");
    std::replace(title.begin(), title.end(), ' ', '-');
    std::replace(title.begin(), title.end(), '_', '-');
    std::erase_if(title, [](unsigned char c) { return !std::isalnum(c) && c != '-'; });
    title.erase(title.begin(),
                std::find_if(title.begin(), title.end(), [](char ch) { return ch != '-'; }));
    title.erase(std::find_if(title.rbegin(), title.rend(), [](char ch) { return ch != '-'; }).base(),
                title.end());
    title.erase(std::unique(title.begin(), title.end(),
                            [](char a, char b) { return a == '-' && b == '-'; }),
                title.end());
    return title;
}

std::string UrlEncode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

std::optional<std::string> BoundedHttpGet(const std::string& full_url,
                                          const std::shared_ptr<std::atomic_bool>& cancel) {
    if (cancel && cancel->load()) {
        return std::nullopt;
    }
    const auto scheme_end = full_url.find("://");
    if (scheme_end == std::string::npos) {
        return std::nullopt;
    }
    const auto path_start = full_url.find('/', scheme_end + 3);
    const std::string host =
        path_start == std::string::npos ? full_url : full_url.substr(0, path_start);
    const std::string path =
        path_start == std::string::npos ? std::string{"/"} : full_url.substr(path_start);
    try {
        httplib::Client client(host);
        client.set_connection_timeout(HttpTimeoutSeconds);
        client.set_read_timeout(HttpTimeoutSeconds);
        client.set_write_timeout(HttpTimeoutSeconds);
        client.set_follow_location(true);
#ifdef YUZU_BUNDLED_OPENSSL
        client.load_ca_cert_store(kCert, sizeof(kCert));
#endif
        httplib::Headers headers{
            {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
                           "Chrome/131.0.0.0 Safari/537.36"},
        };
        auto res = client.Get(path, headers);
        if (!res || res->status != 200 || res->body.empty()) {
            return std::nullopt;
        }
        return std::move(res->body);
    } catch (const std::exception& e) {
        LOG_WARNING(Frontend, "NupMetadata: HTTP GET failed for {}: {}", full_url, e.what());
        return std::nullopt;
    }
}

int ScoreCandidateTitle(const std::string& candidate, const std::string& query) {
    const std::string c_slug = NormalizeSlug(candidate);
    const std::string q_slug = NormalizeSlug(query);
    if (c_slug.empty() || q_slug.empty()) {
        return -1000;
    }
    if (c_slug == q_slug) {
        return 1000;
    }
    int score = 0;
    if (c_slug.starts_with(q_slug + "-") || q_slug.starts_with(c_slug + "-")) {
        score += 750 - static_cast<int>(std::abs(static_cast<long>(c_slug.size()) -
                                                 static_cast<long>(q_slug.size())));
    } else if (c_slug.find(q_slug) != std::string::npos) {
        score += 600;
    }
    const auto split_words = [](const std::string& s) {
        std::set<std::string> words;
        std::size_t start = 0;
        while (start < s.size()) {
            const auto dash = s.find('-', start);
            const auto word = s.substr(start, dash - start);
            if (!word.empty()) {
                words.insert(word);
            }
            if (dash == std::string::npos) {
                break;
            }
            start = dash + 1;
        }
        return words;
    };
    const auto q_words = split_words(q_slug);
    const auto c_words = split_words(c_slug);
    int overlap = 0;
    for (const auto& w : q_words) {
        if (c_words.contains(w)) {
            ++overlap;
        }
    }
    score += overlap * 120 - static_cast<int>(c_words.size()) * 10;
    const std::string lower_cand = Common::ToLower(candidate);
    if (lower_cand.find("(series)") != std::string::npos ||
        lower_cand.find("(character)") != std::string::npos ||
        lower_cand.find("soundtrack") != std::string::npos) {
        score -= 400;
    }
    return score;
}

} // namespace

bool IsArchiveWritable(const fs::path& archive_path, QString* error_out) {
    std::error_code ec;
    if (!fs::exists(archive_path, ec) || !fs::is_regular_file(archive_path, ec)) {
        if (error_out) {
            *error_out = QObject::tr("The file does not exist or is not a regular file.");
        }
        return false;
    }
    const auto perms = fs::status(archive_path, ec).permissions();
    if ((perms & fs::perms::owner_write) == fs::perms::none) {
        if (error_out) {
            *error_out = QObject::tr("File is read-only (permission denied).");
        }
        return false;
    }
    Common::FS::IOFile file{archive_path, Common::FS::FileAccessMode::ReadWrite};
    if (!file.IsOpen()) {
        if (error_out) {
            *error_out = QObject::tr("Cannot open file for writing. Check file ownership and permissions.");
        }
        return false;
    }
    return true;
}

bool IsArchiveRunning(Core::System& system, const fs::path& archive_path) {
    if (!system.IsPoweredOn()) {
        return false;
    }
    std::error_code ec;
    const auto canonical_archive = fs::weakly_canonical(archive_path, ec);
    return false;
}

std::vector<u8> NormalizeAndEncodeIcon(const QImage& source) {
    if (source.isNull()) {
        return {};
    }
    if (source.width() <= 0 || source.height() <= 0 ||
        source.width() > MaxDecodeDimension || source.height() > MaxDecodeDimension) {
        LOG_WARNING(Frontend, "NupMetadata: Source image dimension out of safe bounds: {}x{}",
                    source.width(), source.height());
        return {};
    }
    QImage img = source.convertToFormat(QImage::Format_RGBA8888);
    if (img.width() != TargetIconSize || img.height() != TargetIconSize) {
        img = img.scaled(TargetIconSize, TargetIconSize, Qt::KeepAspectRatioByExpanding,
                         Qt::SmoothTransformation);
        if (img.width() > TargetIconSize || img.height() > TargetIconSize) {
            const int x = (img.width() - TargetIconSize) / 2;
            const int y = img.height() > img.width() ? (img.height() - TargetIconSize) / 3
                                                     : (img.height() - TargetIconSize) / 2;
            img = img.copy(x, y, TargetIconSize, TargetIconSize);
        }
    }
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    return std::vector<u8>(encoded.begin(), encoded.end());
}

std::string ResolveTitleAlias(const std::string& raw_name, u64 title_id) {
    static const std::unordered_map<u64, std::string> by_title_id{
        {0x0400C3F00006E000ULL, "Mario Kart World"},
        {0x0100000000010000ULL, "Super Mario Odyssey"},
        {0x0100152000022000ULL, "Mario Kart 8 Deluxe"},
        {0x01007EF00011E000ULL, "The Legend of Zelda: Breath of the Wild"},
        {0x0100F2C0115B6000ULL, "The Legend of Zelda: Tears of the Kingdom"},
        {0x01006A800016E000ULL, "Super Smash Bros. Ultimate"},
        {0x01006F8002326000ULL, "Animal Crossing: New Horizons"},
    };
    static const std::unordered_map<std::string, std::string> by_alias{
        {"mkw", "Mario Kart World"},
        {"mariokartworld", "Mario Kart World"},
        {"mario-kart-world", "Mario Kart World"},
        {"dkb", "Donkey Kong Bananza"},
        {"donkeykongbananza", "Donkey Kong Bananza"},
        {"kar", "Kirby Air Riders"},
        {"kirbyairriders", "Kirby Air Riders"},
        {"mp4", "Metroid Prime 4: Beyond"},
        {"metroidprime4", "Metroid Prime 4: Beyond"},
        {"plza", "Pokemon Legends: Z-A"},
        {"pokemonlegendsza", "Pokemon Legends: Z-A"},
        {"mk8", "Mario Kart 8 Deluxe"},
        {"mk8d", "Mario Kart 8 Deluxe"},
        {"smo", "Super Mario Odyssey"},
        {"botw", "The Legend of Zelda: Breath of the Wild"},
        {"totk", "The Legend of Zelda: Tears of the Kingdom"},
        {"ssbu", "Super Smash Bros. Ultimate"},
        {"acnh", "Animal Crossing: New Horizons"},
        {"smbw", "Super Mario Bros. Wonder"},
        {"splatoon3", "Splatoon 3"},
        {"xc3", "Xenoblade Chronicles 3"},
        {"lm3", "Luigi's Mansion 3"},
        {"pikmin4", "Pikmin 4"},
    };
    const std::string slug = NormalizeSlug(raw_name);
    if (const auto it = by_alias.find(slug); it != by_alias.end()) {
        return it->second;
    }
    if (slug.empty()) {
        if (const auto it = by_title_id.find(title_id); it != by_title_id.end()) {
            return it->second;
        }
    }
    return raw_name;
}

void DetectIconAsync(const std::string& query_title, u64 title_id,
                     const fs::path& archive_path,
                     std::shared_ptr<std::atomic_bool> cancel_token,
                     std::function<void(DetectResult)> on_complete) {
    (void)QtConcurrent::run([query_title, title_id, archive_path, cancel_token, on_complete] {
        DetectResult result;
        const auto deliver = [&](DetectResult r) {
            if (cancel_token && cancel_token->load()) {
                return;
            }
            QMetaObject::invokeMethod(qApp, [on_complete, r] { on_complete(r); });
        };
        try {
            const auto start_time = std::chrono::steady_clock::now();
            const auto is_timed_out = [&] {
                return (std::chrono::steady_clock::now() - start_time) > OverallTimeout;
            };
        const std::string search_name = ResolveTitleAlias(query_title, title_id);
        if (search_name != query_title) {
            result.resolved_title = search_name;
        }
        const std::string slug = NormalizeSlug(search_name);

        const auto try_decode = [&](std::string_view bytes) -> bool {
            if (bytes.empty() || (cancel_token && cancel_token->load())) {
                return false;
            }
            QImageReader reader;
            QByteArray qbytes(bytes.data(), static_cast<int>(bytes.size()));
            QBuffer buffer(&qbytes);
            buffer.open(QIODevice::ReadOnly);
            reader.setDevice(&buffer);
            reader.setAllocationLimit(64);
            const QImage img = reader.read();
            if (img.isNull()) {
                return false;
            }
            result.icon_bytes = NormalizeAndEncodeIcon(img);
            if (!result.icon_bytes.empty()) {
                result.success = true;
                return true;
            }
            return false;
        };

        // 1. Local image file next to archive (< 1ms)
        if (!archive_path.empty() && archive_path.has_parent_path()) {
            const auto dir = archive_path.parent_path();
            const auto stem = Common::FS::PathToUTF8String(archive_path.stem());
            for (const auto& base : {slug, stem, std::string{"icon"}}) {
                if (base.empty() || (cancel_token && cancel_token->load())) {
                    break;
                }
                for (const char* ext : {".png", ".jpg", ".jpeg", ".webp"}) {
                    const auto candidate = dir / (base + ext);
                    std::error_code ec;
                    if (fs::is_regular_file(candidate, ec)) {
                        QImageReader local_reader(
                            QString::fromStdString(Common::FS::PathToUTF8String(candidate)));
                        local_reader.setAllocationLimit(64);
                        const QImage img = local_reader.read();
                        if (!img.isNull()) {
                            result.icon_bytes = NormalizeAndEncodeIcon(img);
                            if (!result.icon_bytes.empty()) {
                                result.success = true;
                                deliver(std::move(result));
                                return;
                            }
                        }
                    }
                }
            }
        }

        // 2. Eden official boxart repository (fast single GET)
        if (!slug.empty() && !is_timed_out() && (!cancel_token || !cancel_token->load())) {
            std::vector<std::string> candidate_slugs{slug};
            if (!slug.starts_with("the-legend-of-")) {
                candidate_slugs.push_back("the-legend-of-" + slug);
            }
            if (!slug.starts_with("super-")) {
                candidate_slugs.push_back("super-" + slug);
            }
            for (const auto& s : candidate_slugs) {
                if (is_timed_out() || (cancel_token && cancel_token->load())) {
                    break;
                }
                const auto url = fmt::format(
                    "https://raw.githubusercontent.com/eden-emulator/boxart/refs/heads/master/img/{}.png",
                    s);
                if (const auto body = BoundedHttpGet(url, cancel_token); body && try_decode(*body)) {
                    deliver(std::move(result));
                    return;
                }
            }
        }

        // 3. Wikipedia API (single search request, 1 top candidate image)
        if (!search_name.empty() && !is_timed_out() && (!cancel_token || !cancel_token->load())) {
            const std::string api_url = fmt::format(
                "https://en.wikipedia.org/w/api.php?action=query&format=json&generator=search"
                "&gsrsearch={}&gsrlimit=4&prop=pageimages&piprop=thumbnail|original"
                "&pithumbsize=512&pilicense=any",
                UrlEncode(search_name + " video game"));
            if (const auto json_body = BoundedHttpGet(api_url, cancel_token)) {
                try {
                    const auto parsed = nlohmann::json::parse(*json_body, nullptr, false);
                    if (!parsed.is_discarded() && parsed.contains("query") &&
                        parsed["query"].contains("pages")) {
                        std::string best_img_url;
                        int best_score = -9999;
                        for (const auto& [_, page] : parsed["query"]["pages"].items()) {
                            std::string img_url;
                            if (page.contains("thumbnail") && page["thumbnail"].contains("source") &&
                                page["thumbnail"]["source"].is_string()) {
                                img_url = page["thumbnail"]["source"].get<std::string>();
                            } else if (page.contains("original") && page["original"].contains("source") &&
                                       page["original"]["source"].is_string()) {
                                img_url = page["original"]["source"].get<std::string>();
                            }
                            if (img_url.empty()) {
                                continue;
                            }
                            const std::string page_title = page.value("title", std::string{});
                            const int score = ScoreCandidateTitle(page_title, search_name);
                            if (score > best_score) {
                                best_score = score;
                                best_img_url = std::move(img_url);
                            }
                        }
                        if (!best_img_url.empty() && !is_timed_out() &&
                            (!cancel_token || !cancel_token->load())) {
                            if (const auto img_body = BoundedHttpGet(best_img_url, cancel_token);
                                img_body && try_decode(*img_body)) {
                                deliver(std::move(result));
                                return;
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    LOG_WARNING(Frontend, "NupMetadata: JSON error: {}", e.what());
                }
            }
        }

        if (!result.success) {
            result.error_message =
                QObject::tr("Could not find a matching icon for \"%1\".")
                    .arg(QString::fromStdString(search_name));
        }
        deliver(std::move(result));
        } catch (const std::exception& e) {
            result.success = false;
            result.error_message = QString::fromStdString(e.what());
            deliver(std::move(result));
        } catch (...) {
            result.success = false;
            result.error_message = QObject::tr("An unexpected error occurred during icon detection.");
            deliver(std::move(result));
        }
    });
}

bool CommitMetadataChanges(const fs::path& archive_path,
                           const std::optional<std::string>& new_title,
                           const std::optional<std::vector<u8>>& new_icon,
                           QString* error_out) {
    if (!new_title && !new_icon) {
        return true;
    }
    QString write_err;
    if (!IsArchiveWritable(archive_path, &write_err)) {
        if (error_out) {
            *error_out = write_err;
        }
        return false;
    }
    std::string error;
    FileSys::Nup::MetadataUpdate update;
    if (new_title) {
        update.title_name = *new_title;
    }
    if (new_icon) {
        update.icon = *new_icon;
    }
    const auto res = FileSys::Nup::UpdateMetadata(archive_path, update, &error);
    if (res != FileSys::Nup::PackResult::Success) {
        if (error_out) {
            *error_out = QString::fromStdString(error);
        }
        return false;
    }
    return true;
}

bool HasRecoveryBackup(const fs::path& archive_path) {
    return FileSys::Nup::HasMetadataBackup(archive_path);
}

bool RestoreRecoveryBackup(const fs::path& archive_path, QString* error_out) {
    std::string err;
    const bool ok = FileSys::Nup::RestoreMetadataBackup(archive_path, &err);
    if (!ok && error_out) {
        *error_out = QString::fromStdString(err);
    }
    return ok;
}

} // namespace NupMetadata
