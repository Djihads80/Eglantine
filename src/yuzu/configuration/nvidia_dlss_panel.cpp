// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef EGLANTINE_STREAMLINE

#include "common/httplib.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>
#include <fmt/format.h>
#include <zlib.h>

#include "common/common_types.h"
#include "common/fs/fs_util.h"
#include "common/logging.h"
#include "video_core/vulkan_common/streamline.h"
#include "yuzu/configuration/nvidia_dlss_panel.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace {

namespace Streamline = Vulkan::Streamline;

constexpr const char* RELEASE_PAGE = "https://github.com/NVIDIA-RTX/Streamline/releases";
constexpr const char* RELEASE_VERSION = "2.14.16";
constexpr const char* ZIP_HOST = "https://github.com";
constexpr const char* ZIP_PATH =
    "/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip";
constexpr const char* INTERPOSER_ENTRY = "bin/x64/sl.interposer.dll";
constexpr const char* MODEL_ENTRY = "bin/x64/nvngx_dlss.dll";
constexpr const char* STREAMLINE_LICENSE_ENTRY = "license.txt";
constexpr const char* DLSS_LICENSE_ENTRY = "bin/x64/nvngx_dlss.license.txt";

QString Tr(const char* text) {
    return QCoreApplication::translate("NvidiaDlssPanel", text);
}

u16 U16(const std::string& data, size_t at) {
    return static_cast<u16>(static_cast<u8>(data[at]) | static_cast<u8>(data[at + 1]) << 8);
}

u32 U32(const std::string& data, size_t at) {
    return static_cast<u32>(U16(data, at)) | static_cast<u32>(U16(data, at + 2)) << 16;
}

u64 U64(const std::string& data, size_t at) {
    return static_cast<u64>(U32(data, at)) | static_cast<u64>(U32(data, at + 4)) << 32;
}

struct ZipEntry {
    u16 method{};
    u32 crc{};
    u64 compressed{};
    u64 size{};
    u64 offset{};
};

/// Reads single files out of a zip on a web server with range requests, so only the files
/// needed are downloaded (about 32 MB of the 276 MB release).
class RemoteZip {
public:
    /// Follows GitHub's redirect to the file's (short-lived) download address.
    bool Resolve() {
        host = ZIP_HOST;
        path = ZIP_PATH;
        for (int redirect = 0; redirect < 5; ++redirect) {
            const auto result = MakeClient()->Get(path, httplib::Headers{{"Range", "bytes=0-0"}});
            if (!result) {
                error = httplib::to_string(result.error());
                return false;
            }
            if (result->status == 206) {
                // "bytes 0-0/<size>"; the server doesn't take "last N bytes" ranges.
                const std::string range = result->get_header_value("Content-Range");
                const size_t slash = range.rfind('/');
                zip_size = slash == std::string::npos
                               ? 0
                               : std::strtoull(range.c_str() + slash + 1, nullptr, 10);
                if (zip_size == 0) {
                    error = "no file size";
                    return false;
                }
                return true;
            }
            const std::string location = result->get_header_value("Location");
            const size_t scheme = location.find("://");
            const size_t slash =
                scheme == std::string::npos ? std::string::npos : location.find('/', scheme + 3);
            if (result->status < 300 || result->status >= 400 || slash == std::string::npos) {
                error = fmt::format("HTTP {}", result->status);
                return false;
            }
            host = location.substr(0, slash);
            path = location.substr(slash);
        }
        error = "too many redirects";
        return false;
    }

    bool Open() {
        if (!Resolve()) {
            return false;
        }
        const u64 tail_start = zip_size > 65558 ? zip_size - 65558 : 0;
        const std::optional<std::string> tail =
            Fetch(fmt::format("bytes={}-{}", tail_start, zip_size - 1));
        if (!tail) {
            return false;
        }
        const size_t end = tail->rfind(std::string{"PK\x05\x06", 4});
        if (end == std::string::npos || end + 22 > tail->size()) {
            error = "no zip directory";
            return false;
        }
        u64 directory_size = U32(*tail, end + 12);
        u64 directory_offset = U32(*tail, end + 16);
        if (directory_offset == 0xFFFFFFFF && end >= 20 &&
            tail->compare(end - 20, 4, std::string{"PK\x06\x07", 4}) == 0) {
            const u64 record = U64(*tail, end - 20 + 8);
            const std::optional<std::string> zip64 =
                Fetch(fmt::format("bytes={}-{}", record, record + 55));
            if (!zip64 || zip64->size() < 56) {
                return false;
            }
            directory_size = U64(*zip64, 40);
            directory_offset = U64(*zip64, 48);
        }
        const std::optional<std::string> directory = Fetch(
            fmt::format("bytes={}-{}", directory_offset, directory_offset + directory_size - 1));
        if (!directory) {
            return false;
        }
        for (size_t at = 0; at + 46 <= directory->size() && U32(*directory, at) == 0x02014b50;) {
            const u16 name_length = U16(*directory, at + 28);
            const u16 extra_length = U16(*directory, at + 30);
            const u16 comment_length = U16(*directory, at + 32);
            ZipEntry entry{
                .method = U16(*directory, at + 10),
                .crc = U32(*directory, at + 16),
                .compressed = U32(*directory, at + 20),
                .size = U32(*directory, at + 24),
                .offset = U32(*directory, at + 42),
            };
            const size_t extra_end = at + 46 + name_length + extra_length;
            if (extra_end + comment_length > directory->size()) {
                error = "bad zip directory";
                return false;
            }
            const std::string name = directory->substr(at + 46, name_length);
            // Zip64 sizes and offset, in this order, for the fields that overflowed.
            for (size_t extra = at + 46 + name_length; extra + 4 <= extra_end;) {
                const u16 id = U16(*directory, extra);
                const u16 length = U16(*directory, extra + 2);
                const size_t record_end = std::min<size_t>(extra + 4 + length, extra_end);
                if (id == 0x0001) {
                    size_t field = extra + 4;
                    for (u64* value : {&entry.size, &entry.compressed, &entry.offset}) {
                        if (*value == 0xFFFFFFFF && field + 8 <= record_end) {
                            *value = U64(*directory, field);
                            field += 8;
                        }
                    }
                }
                extra += 4 + length;
            }
            entries.emplace(name, entry);
            at += 46 + name_length + extra_length + comment_length;
        }
        return true;
    }

    const ZipEntry* Find(const std::string& name) const {
        const auto it = entries.find(name);
        return it == entries.end() ? nullptr : &it->second;
    }

    std::optional<std::string> Read(const ZipEntry& entry,
                                    const std::function<bool(u64)>& progress = {}) {
        const std::optional<std::string> header =
            Fetch(fmt::format("bytes={}-{}", entry.offset, entry.offset + 29));
        if (!header) {
            return std::nullopt;
        }
        if (header->size() < 30 || U32(*header, 0) != 0x04034b50) {
            error = "bad zip entry";
            return std::nullopt;
        }
        const u64 start = entry.offset + 30 + U16(*header, 26) + U16(*header, 28);
        std::string data;
        if (entry.compressed != 0) {
            std::optional<std::string> fetched =
                Fetch(fmt::format("bytes={}-{}", start, start + entry.compressed - 1), progress);
            if (!fetched) {
                return std::nullopt;
            }
            data = std::move(*fetched);
        }
        std::string out;
        if (entry.method == 0) {
            out = std::move(data);
        } else if (entry.method == 8) {
            out.resize(entry.size);
            z_stream stream{};
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
                error = "zlib";
                return std::nullopt;
            }
            stream.next_in = reinterpret_cast<Bytef*>(data.data());
            stream.avail_in = static_cast<uInt>(data.size());
            stream.next_out = reinterpret_cast<Bytef*>(out.data());
            stream.avail_out = static_cast<uInt>(out.size());
            const int result = inflate(&stream, Z_FINISH);
            inflateEnd(&stream);
            if (result != Z_STREAM_END || stream.total_out != entry.size) {
                error = "could not unpack";
                return std::nullopt;
            }
        } else {
            error = "unknown zip compression";
            return std::nullopt;
        }
        const uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(out.data()),
                                static_cast<uInt>(out.size()));
        if (crc != entry.crc) {
            error = "damaged download (checksum)";
            return std::nullopt;
        }
        return out;
    }

    bool canceled{};
    std::string error;

private:
    std::unique_ptr<httplib::Client> MakeClient() const {
        auto client = std::make_unique<httplib::Client>(host);
        client->set_connection_timeout(30);
        client->set_read_timeout(30);
        client->set_write_timeout(30);
#ifdef YUZU_BUNDLED_OPENSSL
        client->load_ca_cert_store(kCert, sizeof(kCert));
#endif
        return client;
    }

    std::optional<std::string> Fetch(const std::string& range,
                                     const std::function<bool(u64)>& progress = {}) {
        std::string body;
        int status = 0;
        const auto result = MakeClient()->Get(
            path, httplib::Headers{{"Range", range}},
            [&status](const httplib::Response& response) {
                status = response.status;
                return response.status == 206;
            },
            [&](const char* data, size_t length) {
                body.append(data, length);
                if (progress && !progress(body.size())) {
                    canceled = true;
                    return false;
                }
                return true;
            });
        if (!result || status != 206) {
            error = result ? fmt::format("HTTP {}", status) : httplib::to_string(result.error());
            return std::nullopt;
        }
        return body;
    }

    std::string host;
    std::string path;
    u64 zip_size{};
    std::map<std::string, ZipEntry> entries;
};

QString Named(const QString& name, const std::string& version) {
    return version.empty() ? name : QStringLiteral("%1 %2").arg(name, QString::fromStdString(version));
}

QString PathText(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

} // Anonymous namespace

NvidiaDlssPanel::NvidiaDlssPanel(QWidget* parent) : QWidget(parent) {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 0, 0, 8);
    layout->setSpacing(6);

    QHBoxLayout* status_row = new QHBoxLayout;
    status_row->setSpacing(8);
    icon_label = new QLabel(this);
    icon_label->setTextFormat(Qt::RichText);
    icon_label->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    icon_label->setFixedWidth(16);
    status_label = new QLabel(this);
    status_label->setTextFormat(Qt::RichText);
    status_label->setWordWrap(true);
    status_row->addWidget(icon_label);
    status_row->addWidget(status_label, 1);

    QHBoxLayout* button_row = new QHBoxLayout;
    button_row->setContentsMargins(24, 0, 0, 0);
    download_button = new QPushButton(this);
    folder_button = new QPushButton(Tr("Open Folder"), this);
    QLabel* about = new QLabel(
        QStringLiteral("<a href=\"%1\">%2</a>").arg(QString::fromLatin1(RELEASE_PAGE),
                                                   Tr("Where do these files come from?")),
        this);
    about->setOpenExternalLinks(true);
    about->setToolTip(Tr("NVIDIA's Streamline releases on GitHub. The files are in bin\\x64 of "
                         "the release zip."));
    button_row->addWidget(download_button);
    button_row->addWidget(folder_button);
    button_row->addStretch(1);
    button_row->addWidget(about);

    layout->addLayout(status_row);
    layout->addLayout(button_row);

    connect(download_button, &QPushButton::clicked, this, [this] { Download(); });
    connect(folder_button, &QPushButton::clicked, this, [] {
        const std::filesystem::path folder = Streamline::FindDlssFiles().folder;
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        QDesktopServices::openUrl(QUrl::fromLocalFile(PathText(folder)));
    });
    Refresh();
}

void NvidiaDlssPanel::showEvent(QShowEvent* event) {
    Refresh();
    QWidget::showEvent(event);
}

void NvidiaDlssPanel::Refresh() {
    const Streamline::DlssFiles files = Streamline::FindDlssFiles();
    const bool has_loader = !files.interposer.empty();
    const bool has_model = !files.model.empty() || !files.app_model.empty();
    const QString dlss = !files.model.empty()
                             ? Named(QStringLiteral("DLSS"), files.model_version)
                             : Tr("%1 from the NVIDIA app")
                                   .arg(Named(QStringLiteral("DLSS"), files.app_model_version));

    QString status;
    QString button;
    if (has_loader && has_model) {
        status = Tr("<b>Ready.</b> Found %1 and %2.")
                     .arg(Named(QStringLiteral("NVIDIA Streamline"), files.interposer_version), dlss);
    } else if (has_model) {
        status = Tr("<b>One file needed:</b> NVIDIA's Streamline loader (sl.interposer.dll). "
                    "%1 will be used. Until then, games use AMD FSR 3.1.")
                     .arg(dlss);
        button = Tr("Download from NVIDIA (1 MB)");
    } else if (has_loader) {
        status = Tr("<b>One file needed:</b> NVIDIA DLSS (nvngx_dlss.dll). Until then, games use "
                    "AMD FSR 3.1.");
        button = Tr("Download from NVIDIA (31 MB)");
    } else {
        status = Tr("<b>NVIDIA's files needed:</b> the Streamline loader and DLSS "
                    "(sl.interposer.dll and nvngx_dlss.dll). Until then, games use AMD FSR 3.1.");
        button = Tr("Download from NVIDIA (32 MB)");
    }
    const bool ready = has_loader && has_model;
    icon_label->setText(ready ? QStringLiteral("<span style=\"color:#2ea043\">&#10004;</span>")
                              : QStringLiteral("<span style=\"color:#d29922\">&#9888;</span>"));
    status_label->setText(status);
    status_label->setToolTip(Tr("Folder: %1").arg(PathText(files.folder)));
    download_button->setText(button);
    download_button->setVisible(!ready);
}

void NvidiaDlssPanel::Download() {
    const Streamline::DlssFiles files = Streamline::FindDlssFiles();
    std::vector<std::string> wanted;
    if (files.interposer.empty()) {
        wanted.emplace_back(INTERPOSER_ENTRY);
    }
    if (files.model.empty() && files.app_model.empty()) {
        wanted.emplace_back(MODEL_ENTRY);
    }
    if (wanted.empty()) {
        return;
    }
    const std::filesystem::path folder = Streamline::UserFolder();
    const auto fail = [this](const std::string& error) {
        LOG_ERROR(Frontend, "NVIDIA DLSS download failed: {}", error);
        QMessageBox::warning(this, Tr("NVIDIA DLSS"),
                             Tr("Could not download NVIDIA's files: %1")
                                 .arg(QString::fromStdString(error)));
        Refresh();
    };

    RemoteZip zip;
    std::string licences;
    {
        QProgressDialog busy(Tr("Contacting NVIDIA's download..."), QString{}, 0, 0, this);
        busy.setWindowTitle(Tr("NVIDIA DLSS"));
        busy.setWindowModality(Qt::WindowModal);
        busy.setMinimumDuration(0);
        busy.show();
        QApplication::processEvents();
        if (!zip.Open()) {
            busy.close();
            fail(zip.error);
            return;
        }
        std::vector<std::pair<QString, const char*>> texts{
            {Tr("NVIDIA Streamline"), STREAMLINE_LICENSE_ENTRY}};
        if (std::ranges::find(wanted, MODEL_ENTRY) != wanted.end()) {
            texts.emplace_back(Tr("NVIDIA DLSS"), DLSS_LICENSE_ENTRY);
        }
        for (const auto& [title, entry_name] : texts) {
            const ZipEntry* const entry = zip.Find(entry_name);
            const std::optional<std::string> text = entry ? zip.Read(*entry) : std::nullopt;
            licences += fmt::format("==== {} ====\n\n{}\n\n", title.toStdString(),
                                    text ? *text : std::string{"(see the release on GitHub)"});
        }
    }

    QStringList names;
    for (const std::string& entry : wanted) {
        names << QString::fromStdString(
            Common::FS::PathToUTF8String(std::filesystem::path{entry}.filename()));
    }
    QDialog dialog(this);
    dialog.setWindowTitle(Tr("Download NVIDIA DLSS"));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QLabel* intro = new QLabel(
        Tr("Eglantine will download %1 from NVIDIA's Streamline %2 release on GitHub into %3.<br>"
           "These files are NVIDIA's, under NVIDIA's licence terms below.")
            .arg(names.join(QStringLiteral(" and ")), QString::fromLatin1(RELEASE_VERSION),
                 PathText(folder)),
        &dialog);
    intro->setWordWrap(true);
    QPlainTextEdit* licence_text = new QPlainTextEdit(QString::fromStdString(licences), &dialog);
    licence_text->setReadOnly(true);
    licence_text->setMinimumSize(640, 380);
    QDialogButtonBox* buttons = new QDialogButtonBox(&dialog);
    buttons->addButton(Tr("Accept and Download"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(intro);
    layout->addWidget(licence_text, 1);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    u64 total = 0;
    std::vector<const ZipEntry*> entries;
    for (const std::string& name : wanted) {
        const ZipEntry* const entry = zip.Find(name);
        if (!entry) {
            fail(name + " is not in the release");
            return;
        }
        entries.push_back(entry);
        total += entry->compressed;
    }
    QProgressDialog progress(Tr("Downloading NVIDIA's DLSS files..."), Tr("Cancel"), 0, 1000,
                             this);
    progress.setWindowTitle(Tr("NVIDIA DLSS"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setValue(0);
    QApplication::processEvents();
    // The download address from the first step only lasts a few minutes.
    if (!zip.Resolve()) {
        progress.close();
        fail(zip.error);
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    u64 done = 0;
    for (size_t i = 0; i < wanted.size(); ++i) {
        const std::optional<std::string> contents = zip.Read(*entries[i], [&](u64 received) {
            progress.setValue(static_cast<int>((done + received) * 1000 / std::max<u64>(total, 1)));
            QApplication::processEvents();
            return !progress.wasCanceled();
        });
        if (!contents) {
            progress.close();
            if (!zip.canceled) {
                fail(zip.error);
            }
            return;
        }
        done += entries[i]->compressed;
        const std::filesystem::path target = folder / std::filesystem::path{wanted[i]}.filename();
        std::filesystem::path partial = target;
        partial += L".download";
        {
            std::ofstream file(partial, std::ios::binary | std::ios::trunc);
            file.write(contents->data(), static_cast<std::streamsize>(contents->size()));
            if (!file) {
                progress.close();
                fail("could not write " + Common::FS::PathToUTF8String(partial));
                return;
            }
        }
        const bool signed_ok = wanted[i] == INTERPOSER_ENTRY ? Streamline::IsStreamlineModule(partial)
                                                             : Streamline::IsSignedByNvidia(partial);
        if (!signed_ok) {
            std::filesystem::remove(partial, ec);
            progress.close();
            fail(Common::FS::PathToUTF8String(target.filename()) + " is not signed by NVIDIA");
            return;
        }
        std::filesystem::rename(partial, target, ec);
        if (ec) {
            std::filesystem::remove(partial, ec);
            progress.close();
            fail("could not save " + Common::FS::PathToUTF8String(target));
            return;
        }
        LOG_INFO(Frontend, "Downloaded {} from NVIDIA's Streamline {} release",
                 Common::FS::PathToUTF8String(target), RELEASE_VERSION);
    }
    progress.setValue(1000);
    progress.close();
    Refresh();
    QMessageBox::information(this, Tr("NVIDIA DLSS"),
                             Tr("NVIDIA DLSS is ready. It is used the next time a game starts."));
}

#endif
