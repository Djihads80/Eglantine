// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QFuture>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QTemporaryFile>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <nxformat.h>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/decrypted_nca.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/loader/loader.h"
#include "core/loader/nx.h"
#include "frontend_common/nx_archive.h"
#include "qt_common/qt_common.h"
#include "yuzu/nup_dialogs.h"
#include "yuzu/nx_tools.h"

namespace {

enum class Kind { None, Package, Nup, Dump };

std::filesystem::path ToPath(const std::string& path) {
    return std::filesystem::path(Common::FS::ToU8String(path));
}

std::string ToUtf8(const std::filesystem::path& path) {
    return Common::FS::PathToUTF8String(path);
}

std::string ToUtf8(const QString& text) {
    return text.toStdString();
}

QString ToQString(const std::filesystem::path& path) {
    return QString::fromStdString(ToUtf8(path));
}

bool HasExtension(const std::string& path, const char* extension) {
    return QString::fromStdString(path).endsWith(QString::fromLatin1(extension),
                                                 Qt::CaseInsensitive);
}

Kind KindOf(const std::string& path) {
    if (FileSys::Nup::DumpRootFromMain(ToPath(path))) {
        return Kind::Dump;
    }
    if (HasExtension(path, ".nup")) {
        return Kind::Nup;
    }
    if (HasExtension(path, ".nsp") || HasExtension(path, ".xci")) {
        return Kind::Package;
    }
    return Kind::None;
}

bool NpdmIsSwitch2(const FileSys::VirtualFile& npdm) {
    FileSys::ProgramMetadata metadata;
    return npdm != nullptr && metadata.Load(npdm) == Loader::ResultStatus::Success &&
           metadata.IsSwitch2();
}

// The platform of a NUP, as the emulator decides it at boot: the NPDM of the newest update
// with an ExeFS, else of the base.
bool NupIsSwitch2(const FileSys::VirtualFile& file) {
    if (!FileSys::NupContainer::IsNupFile(file)) {
        return false;
    }
    FileSys::NupContainer nup{file};
    if (nup.GetStatus() != Loader::ResultStatus::Success) {
        return false;
    }
    const auto base = nup.FindBasePackage();
    const auto update = nup.FindBestUpdatePackage();
    std::optional<std::size_t> program;
    if (update && nup.GetExeFSDir(*update) != nullptr) {
        program = update;
    } else if (base && nup.GetExeFSDir(*base) != nullptr) {
        program = base;
    }
    const auto exefs = program ? nup.GetExeFSDir(*program) : nullptr;
    return NpdmIsSwitch2(exefs != nullptr ? exefs->GetFile("main.npdm") : nullptr);
}

// The platform of a loose dump: the NPDM of the base, or of the first update.
bool DumpIsSwitch2(const FileSys::VirtualFilesystem& vfs, const std::filesystem::path& root) {
    std::vector<std::filesystem::path> candidates{root / "exefs" / "main.npdm",
                                                  root / "base" / "exefs" / "main.npdm"};
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root / "updates", ec), end; !ec && it != end;
         it.increment(ec)) {
        candidates.push_back(it->path() / "exefs" / "main.npdm");
    }
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return NpdmIsSwitch2(vfs->OpenFile(ToUtf8(candidate), FileSys::OpenMode::Read));
        }
    }
    return false;
}

const char* PlatformArgument(bool switch2) {
    return switch2 ? "switch2" : "switch1";
}

// Reads one decrypted package for nxformat. Calls for one source are serialized.
int ReadSource(void* user, uint64_t offset, void* buffer, size_t size) {
    const auto& file = *static_cast<FileSys::VirtualFile*>(user);
    if (offset > file->GetSize() || size > file->GetSize() - offset) {
        return 1;
    }
    return file->Read(static_cast<u8*>(buffer), size, static_cast<std::size_t>(offset)) == size ? 0
                                                                                                : 1;
}

// The update version written in an update folder name: "196608", "v196608" or "[v196608]".
std::optional<u32> VersionFromName(const QString& name) {
    bool ok = false;
    const auto plain = name.toUInt(&ok);
    if (ok) {
        return plain;
    }
    const QRegularExpression pattern(QStringLiteral("(?:^|[^0-9A-Za-z])v([0-9]{1,10})(?:$|[^0-9])"),
                                     QRegularExpression::CaseInsensitiveOption);
    const auto match = pattern.match(name);
    if (match.hasMatch()) {
        const auto value = match.captured(1).toULongLong(&ok);
        if (ok && value <= 0xFFFFFFFFull) {
            return static_cast<u32>(value);
        }
    }
    return std::nullopt;
}

// The title ID written in a package name, as in "Game [0100ABCD12340000][v0].nsp".
std::optional<u64> NameTitleId(const std::filesystem::path& path) {
    static const QRegularExpression pattern(QStringLiteral("\\[([0-9A-Fa-f]{16})\\]"));
    const auto match = pattern.match(ToQString(path.filename()));
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    bool ok = false;
    const u64 id = match.captured(1).toULongLong(&ok, 16);
    return ok ? std::optional<u64>{id} : std::nullopt;
}

// The title IDs a package holds, read from its content metadata.
std::vector<u64> PackageTitleIds(const FileSys::VirtualFile& file, const std::string& path) {
    std::vector<u64> ids;
    if (file == nullptr) {
        return ids;
    }
    std::shared_ptr<FileSys::NSP> nsp;
    if (HasExtension(path, ".xci")) {
        nsp = FileSys::XCI(file).GetSecurePartitionNSP();
    } else {
        nsp = std::make_shared<FileSys::NSP>(file);
    }
    if (nsp != nullptr) {
        for (const auto& [id, contents] : nsp->GetNCAs()) {
            ids.push_back(id);
        }
    }
    return ids;
}

// The title IDs of a package: from its name when written there, else from its contents.
std::vector<u64> TitleIdsOf(const FileSys::VirtualFilesystem& vfs,
                            const std::filesystem::path& path) {
    if (const auto id = NameTitleId(path)) {
        return {*id};
    }
    const auto utf8 = ToUtf8(path);
    return PackageTitleIds(vfs->OpenFile(utf8, FileSys::OpenMode::Read), utf8);
}

// Updates and DLC of the same game found next to a Switch 1 package, sorted by name.
std::vector<std::filesystem::path> RelatedPackages(const FileSys::VirtualFilesystem& vfs,
                                                   const std::filesystem::path& package) {
    std::vector<std::filesystem::path> related;
    const auto own = TitleIdsOf(vfs, package);
    if (own.empty()) {
        return related;
    }
    std::vector<u64> bases;
    for (const u64 id : own) {
        bases.push_back(FileSys::GetBaseTitleID(id));
    }
    for (const auto& path : FrontendCommon::NxArchive::RegularFiles(package.parent_path())) {
        const auto utf8 = ToUtf8(path);
        if (path == package || !(HasExtension(utf8, ".nsp") || HasExtension(utf8, ".xci"))) {
            continue;
        }
        // A name with another game's ID is skipped without opening the file.
        const auto named = NameTitleId(path);
        if (named &&
            std::find(bases.begin(), bases.end(), FileSys::GetBaseTitleID(*named)) == bases.end()) {
            continue;
        }
        const auto ids = TitleIdsOf(vfs, path);
        const bool same_game = std::any_of(ids.begin(), ids.end(), [&](u64 id) {
            return std::find(bases.begin(), bases.end(), FileSys::GetBaseTitleID(id)) !=
                   bases.end();
        });
        // Another copy of the same base is not an update or a DLC.
        const bool other_base = std::all_of(ids.begin(), ids.end(), [&](u64 id) {
            return std::find(own.begin(), own.end(), id) != own.end();
        });
        if (same_game && !other_base) {
            related.push_back(path);
        }
    }
    std::sort(related.begin(), related.end());
    return related;
}

struct Snapshot {
    QString identity;
    QString kind;
    QString title_id;
    QString version;
    QString display_version;
    QString name;
};

// Shows a picture with a square selection: click or drag to move it.
class CropArea final : public QWidget {
public:
    CropArea(const QImage& image_, QWidget* parent) : QWidget(parent), image{image_} {
        const QSize fit = image.size().scaled(360, 360, Qt::KeepAspectRatio);
        setFixedSize(fit);
        scale = static_cast<double>(fit.width()) / image.width();
        side = std::min(image.width(), image.height());
        center = QPointF(image.width() / 2.0, image.height() / 2.0);
        setCursor(Qt::SizeAllCursor);
    }

    // 100: the whole short side; 400: a quarter of it.
    void SetZoom(int percent) {
        side = std::min(image.width(), image.height()) * 100.0 / percent;
        Clamp();
    }

    QRect Selection() const {
        const int s = static_cast<int>(side);
        const int x = std::clamp(static_cast<int>(center.x() - side / 2.0), 0, image.width() - s);
        const int y = std::clamp(static_cast<int>(center.y() - side / 2.0), 0, image.height() - s);
        return QRect(x, y, s, s);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(rect(), image);
        const QRect s = Selection();
        const QRectF shown(s.x() * scale, s.y() * scale, s.width() * scale, s.height() * scale);
        QPainterPath outside;
        outside.addRect(QRectF(rect()));
        outside.addRect(shown);
        painter.fillPath(outside, QColor(0, 0, 0, 140));
        painter.setPen(QPen(Qt::white, 2));
        painter.drawRect(shown);
    }
    void mousePressEvent(QMouseEvent* event) override {
        last = event->position();
        pressed = event->position();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if ((event->buttons() & Qt::LeftButton) == 0) {
            return;
        }
        center += (event->position() - last) / scale;
        last = event->position();
        Clamp();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        // A simple click centers the square on the point.
        if (event->position() == pressed) {
            center = event->position() / scale;
            Clamp();
        }
    }

private:
    void Clamp() {
        center.setX(std::clamp(center.x(), side / 2.0, image.width() - side / 2.0));
        center.setY(std::clamp(center.y(), side / 2.0, image.height() - side / 2.0));
        update();
    }

    QImage image;
    double scale = 1.0;
    double side = 0.0;
    QPointF center;
    QPointF last;
    QPointF pressed;
};

// The square part of the picture to keep.
std::optional<QRect> ChooseCrop(const QImage& image, QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Crop Icon"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(
        QObject::tr("Click or drag to move the square. Zoom to keep a smaller part."), &dialog));
    auto* area = new CropArea(image, &dialog);
    layout->addWidget(area, 0, Qt::AlignCenter);
    auto* zoom_row = new QHBoxLayout;
    zoom_row->addWidget(new QLabel(QObject::tr("Zoom:"), &dialog));
    auto* zoom = new QSlider(Qt::Horizontal, &dialog);
    zoom->setRange(100, 400);
    QObject::connect(zoom, &QSlider::valueChanged, area,
                     [area](int value) { area->SetZoom(value); });
    zoom_row->addWidget(zoom, 1);
    layout->addLayout(zoom_row);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return area->Selection();
}

// The selected square as a size x size JPEG, like a console icon.
QByteArray EncodeIcon(const QImage& source, const QRect& crop, int size) {
    const QImage scaled =
        source.copy(crop).scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage flat(size, size, QImage::Format_RGB32);
    flat.fill(Qt::black);
    QPainter painter(&flat);
    painter.drawImage(0, 0, scaled);
    painter.end();
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!flat.save(&buffer, "JPEG", 92)) {
        return {};
    }
    return bytes;
}

} // namespace

struct NxTools::Job {
    ~Job() {
        for (auto* source : sources) {
            nxformat_source_free(source);
        }
        nxformat_cancel_free(cancel);
    }

    QString title;
    QString done_message;
    // Converted games or dumps the user may send to the Recycle Bin at the end.
    std::vector<std::filesystem::path> originals;
    std::vector<std::string> args;
    // Decrypted views read by nxformat; they live until the run has returned.
    std::vector<std::unique_ptr<FileSys::VirtualFile>> files;
    std::vector<nxformat_source*> sources;
    nxformat_cancel* cancel = nxformat_cancel_new();
    QFuture<void> future;

    std::atomic<u64> total{0};
    std::atomic<u64> written{0};
    QElapsedTimer clock;
    // When nxformat started counting bytes (ms on `clock`), and the smoothed time left.
    qint64 counting_since = -1;
    double seconds_left = -1;
    std::mutex lines_mutex;
    QStringList added;
    QStringList skipped;
    // The new icon given to set-metadata, removed when the job ends.
    std::unique_ptr<QTemporaryFile> icon_file;

    // nxformat 1.0.1 fails pack and add when the rebuild recipe cannot be kept (1.0.0 only
    // warned). Such a failure can be run again without the recipe.
    bool CanRetryWithoutRecipe(const QString& error) const {
        if (args.empty() || (args[0] != "pack" && args[0] != "add") ||
            std::ranges::find(args, std::string{"--no-recipe"}) != args.end()) {
            return false;
        }
        return error.contains(QStringLiteral("recipe"), Qt::CaseInsensitive) ||
               error.contains(QStringLiteral("rebuild"), Qt::CaseInsensitive);
    }

    // Runs the same command again, without the recipe. The decrypted sources stay open.
    void PrepareRetryWithoutRecipe() {
        args.emplace_back("--no-recipe");
        total = 0;
        written = 0;
        counting_since = -1;
        seconds_left = -1;
        // `added` is kept: what the failed run committed stays in the archive.
        std::scoped_lock lock{lines_mutex};
        skipped.clear();
    }

    // Closes the packages nxformat read, so the original can be moved.
    void ReleaseSources() {
        for (auto* source : sources) {
            nxformat_source_free(source);
        }
        sources.clear();
        files.clear();
    }

    // Adds an NSP or XCI to the command, decrypted on the fly. Returns why it cannot be,
    // empty on success.
    QString AddPackage(const FileSys::VirtualFilesystem& filesystem,
                       const std::filesystem::path& path) {
        const auto file = filesystem->OpenFile(ToUtf8(path), FileSys::OpenMode::Read);
        std::string error = "file not found";
        // An XCI goes in as a whole decrypted gamecard image, so nxformat keeps the firmware of
        // its update partition. Other packages go in as a decrypted NSP.
        std::string gamecard_error;
        auto decrypted =
            file != nullptr ? FileSys::OpenDecryptedGamecard(file, gamecard_error) : nullptr;
        const bool gamecard = decrypted != nullptr;
        if (!gamecard && file != nullptr) {
            if (gamecard_error != "not a gamecard image") {
                LOG_WARNING(Frontend,
                            "{}: converted without its gamecard update partition (firmware): {}",
                            ToUtf8(path.filename()), gamecard_error);
            }
            decrypted = FileSys::OpenDecryptedPackage(file, error);
        }
        if (decrypted == nullptr) {
            return QString::fromStdString(error);
        }
        auto& held =
            files.emplace_back(std::make_unique<FileSys::VirtualFile>(std::move(decrypted)));
        const auto name = ToUtf8(path.stem()) + (gamecard ? ".xci" : ".nsp");
        auto* source =
            nxformat_source_new(name.c_str(), (*held)->GetSize(), &ReadSource, held.get());
        if (source == nullptr) {
            return QStringLiteral("out of memory");
        }
        sources.push_back(source);
        args.emplace_back(nxformat_source_path(source));
        return {};
    }
};

namespace {

QString CannotDecrypt(const std::filesystem::path& path, const QString& error) {
    return NxTools::tr("%1 cannot be decrypted: %2\n\nCheck that your keys are installed.")
        .arg(ToQString(path.filename()), error);
}

// Lists an archive with "contents", one tab-split line at a time. Returns the error, empty on
// success.
QString ListContents(const std::string& archive,
                     const std::function<void(const QStringList&)>& on_line) {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const char* args[] = {"contents", archive.c_str()};
    char error[512]{};
    const auto code = nxformat_run(
        args, 2,
        [](void* user, int is_error, const char* line) {
            if (!is_error) {
                (*static_cast<const std::function<void(const QStringList&)>*>(user))(
                    QString::fromUtf8(line).split(QLatin1Char('\t')));
            }
        },
        const_cast<std::function<void(const QStringList&)>*>(&on_line), nullptr, error,
        sizeof(error));
    QApplication::restoreOverrideCursor();
    if (code == NXFORMAT_OK) {
        return {};
    }
    return error[0] != '\0' ? QString::fromUtf8(error) : QStringLiteral("unknown error");
}

} // namespace

NxTools::NxTools(FileSys::VirtualFilesystem vfs_, QWidget* parent)
    : QObject(parent), vfs{std::move(vfs_)}, parent_widget{parent} {}

NxTools::~NxTools() {
    if (running) {
        nxformat_cancel_set(running->cancel);
        running->future.waitForFinished();
    }
}

bool NxTools::CanConvert(const std::string& game_path) {
    return KindOf(game_path) != Kind::None;
}

bool NxTools::IsNxArchive(const std::string& game_path) {
    return HasExtension(game_path, ".nx");
}

void NxTools::ConvertToNx(const std::string& game_path) {
    if (running) {
        return;
    }
    const QString title = tr("Convert to NX");
    const auto kind = KindOf(game_path);
    auto job = std::make_shared<Job>();
    job->title = tr("Converting to NX");

    std::filesystem::path source = ToPath(game_path);
    std::filesystem::path suggested;
    if (kind == Kind::Dump) {
        source = *FileSys::Nup::DumpRootFromMain(source);
        auto name = source.filename();
        name += ".nx";
        suggested = source.parent_path() / name;
    } else {
        suggested = source;
        suggested.replace_extension(".nx");
    }
    QString chosen = QFileDialog::getSaveFileName(parent_widget, title, ToQString(suggested),
                                                  tr("NX archive (*.nx)"));
    if (chosen.isEmpty()) {
        return;
    }
    const auto output = FrontendCommon::NxArchive::WithNxExtension(ToUtf8(chosen));
    chosen = QString::fromStdString(output);
    if (std::error_code ec; std::filesystem::exists(ToPath(output), ec)) {
        QMessageBox::warning(parent_widget, title,
                             tr("The archive already exists:\n%1").arg(chosen));
        return;
    }

    switch (kind) {
    case Kind::Package: {
        // Switch 1 NSP or XCI: decrypted on the fly, as a DNSP view of the package.
        job->args = {"pack"};
        if (const auto error = job->AddPackage(vfs, source); !error.isEmpty()) {
            QMessageBox::warning(parent_widget, title, CannotDecrypt(source, error));
            return;
        }
        job->originals.push_back(source);

        // Updates and DLC of the same game next to it go into the same archive.
        const auto related = RelatedPackages(vfs, source);
        if (!related.empty()) {
            QDialog dialog(parent_widget);
            dialog.setWindowTitle(title);
            auto* layout = new QVBoxLayout(&dialog);
            layout->addWidget(
                new QLabel(tr("Updates and DLC of this game were found in the same folder.\n"
                              "The checked ones are added to the NX archive:"),
                           &dialog));
            auto* list = new QListWidget(&dialog);
            for (const auto& related_path : related) {
                auto* item = new QListWidgetItem(ToQString(related_path.filename()), list);
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(Qt::Checked);
            }
            list->setMinimumWidth(560);
            layout->addWidget(list);
            auto* buttons =
                new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            layout->addWidget(buttons);
            if (dialog.exec() != QDialog::Accepted) {
                return;
            }
            for (std::size_t i = 0; i < related.size(); ++i) {
                if (list->item(static_cast<int>(i))->checkState() != Qt::Checked) {
                    continue;
                }
                if (const auto error = job->AddPackage(vfs, related[i]); !error.isEmpty()) {
                    QMessageBox::warning(parent_widget, title, CannotDecrypt(related[i], error));
                    return;
                }
                job->originals.push_back(related[i]);
            }
        }
        job->args.push_back(output);
        job->args.push_back("--frontend");
        break;
    }
    case Kind::Nup: {
        const auto file = vfs->OpenFile(game_path, FileSys::OpenMode::Read);
        job->args = {
            "pack-nup",   game_path,   output, "--platform", PlatformArgument(NupIsSwitch2(file)),
            "--frontend", "--no-trail"};
        break;
    }
    case Kind::Dump:
        job->args = {"pack-tree",
                     ToUtf8(source),
                     output,
                     "--platform",
                     PlatformArgument(DumpIsSwitch2(vfs, source)),
                     "--frontend",
                     "--no-trail"};
        break;
    case Kind::None:
        return;
    }
    job->done_message = tr("The NX archive was created:\n%1").arg(chosen);
    if (job->originals.empty()) {
        job->originals.push_back(source);
    }
    Start(std::move(job));
}

void NxTools::AddToNx(const std::string& archive_path) {
    if (running) {
        return;
    }
    const QString title = tr("Add to NX");
    const auto archive = vfs->OpenFile(archive_path, FileSys::OpenMode::Read);
    const auto platform = archive != nullptr ? Loader::AppLoader_NX::ReadPlatform(archive, 0) : 0;
    if (platform == 0) {
        QMessageBox::warning(parent_widget, title, tr("This NX archive cannot be read."));
        return;
    }
    const bool switch2 = platform == 2;

    QMessageBox choice(QMessageBox::Question, title,
                       tr("Add an update or DLC to this NX archive. The archive is changed in "
                          "place; the content it already holds is kept."),
                       QMessageBox::Cancel, parent_widget);
    auto* folder_button = choice.addButton(tr("Update Folder..."), QMessageBox::AcceptRole);
    auto* file_button = choice.addButton(tr("NSP or XCI Files..."), QMessageBox::AcceptRole);
    if (switch2) {
        // Native Switch 1 packages only; a Switch 2 game takes extracted update folders.
        file_button->setVisible(false);
    }
    choice.exec();

    auto job = std::make_shared<Job>();
    job->title = tr("Adding to NX");
    if (choice.clickedButton() == folder_button) {
        const QString chosen =
            QFileDialog::getExistingDirectory(parent_widget, tr("Select the Update Folder"),
                                              ToQString(ToPath(archive_path).parent_path()));
        if (chosen.isEmpty()) {
            return;
        }
        const QString folder =
            ToQString(FrontendCommon::NxArchive::UpdateFolder(ToPath(ToUtf8(chosen))));
        job->args = {
            "add-tree",   archive_path, ToUtf8(folder), "--platform", PlatformArgument(switch2),
            "--frontend", "--no-trail"};
        // A folder that holds exefs/ itself is one update; its version comes from the name.
        if (QFileInfo(folder + QStringLiteral("/exefs")).isDir()) {
            auto version = VersionFromName(QFileInfo(folder).fileName());
            if (!version) {
                bool ok = false;
                const int value = QInputDialog::getInt(
                    parent_widget, title,
                    tr("Version number of this update (for example 196608 for v196608):"), 65536, 1,
                    std::numeric_limits<int>::max(), 1, &ok);
                if (!ok) {
                    return;
                }
                version = static_cast<u32>(value);
            }
            job->args.push_back("--update-version");
            job->args.push_back(std::to_string(*version));
        }
    } else if (choice.clickedButton() == file_button) {
        const QStringList files = QFileDialog::getOpenFileNames(
            parent_widget, tr("Select Updates or DLC"),
            ToQString(ToPath(archive_path).parent_path()), tr("Switch packages (*.nsp *.xci)"));
        if (files.isEmpty()) {
            return;
        }
        job->args = {"add", archive_path};
        for (const auto& name : files) {
            const auto path = ToPath(ToUtf8(name));
            if (const auto error = job->AddPackage(vfs, path); !error.isEmpty()) {
                QMessageBox::warning(parent_widget, title, CannotDecrypt(path, error));
                return;
            }
        }
        job->args.push_back("--frontend");
    } else {
        return;
    }
    job->done_message = tr("The NX archive was updated.");
    Start(std::move(job));
}

void NxTools::RemoveFromNx(const std::string& archive_path) {
    if (running) {
        return;
    }
    const QString title = tr("Remove from NX");
    std::vector<Snapshot> snapshots;
    if (const auto error =
            ListContents(archive_path,
                         [&](const QStringList& fields) {
                             if (fields.size() >= 8 && fields[0] == QStringLiteral("@SNAPSHOT")) {
                                 snapshots.push_back({fields[1], fields[2], fields[4], fields[5],
                                                      fields[6], fields[7]});
                             }
                         });
        !error.isEmpty()) {
        QMessageBox::warning(parent_widget, title,
                             tr("This NX archive cannot be read: %1").arg(error));
        return;
    }
    std::erase_if(snapshots, [](const Snapshot& s) { return s.kind == QStringLiteral("base"); });
    if (snapshots.empty()) {
        QMessageBox::information(parent_widget, title,
                                 tr("This NX archive holds no update or DLC to remove."));
        return;
    }

    QDialog dialog(parent_widget);
    dialog.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Select the content to remove from the archive:"), &dialog));
    auto* list = new QListWidget(&dialog);
    for (const auto& s : snapshots) {
        QString text = s.kind == QStringLiteral("dlc")
                           ? tr("DLC %1 (v%2)").arg(s.title_id.toUpper(), s.version)
                           : tr("Update %1 (v%2)").arg(s.display_version, s.version);
        if (s.kind == QStringLiteral("dlc") && !s.name.isEmpty()) {
            text += QStringLiteral(" - ") + s.name;
        }
        auto* item = new QListWidgetItem(text, list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        item->setData(Qt::UserRole, s.identity);
    }
    layout->addWidget(list);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Remove"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(480, 320);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    auto job = std::make_shared<Job>();
    job->title = tr("Removing from NX");
    job->args = {"remove", archive_path};
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->checkState() == Qt::Checked) {
            job->args.push_back(ToUtf8(list->item(i)->data(Qt::UserRole).toString()));
        }
    }
    if (job->args.size() == 2) {
        return;
    }
    job->args.push_back("--frontend");
    job->done_message = tr("The selected content was removed from the NX archive.");
    Start(std::move(job));
}

void NxTools::EditNxMetadata(const std::string& archive_path,
                             const std::function<bool()>& prepare_write) {
    if (running) {
        return;
    }
    const QString title = tr("Edit NX Metadata");

    // The application ID, from the archive's own listing.
    QString application_id;
    auto error = ListContents(archive_path, [&](const QStringList& fields) {
        if (application_id.isEmpty() && fields.size() >= 2 && fields[0] == QStringLiteral("@APP")) {
            application_id = fields[1];
        }
    });
    if (error.isEmpty() && application_id.isEmpty()) {
        error = QStringLiteral("no application");
    }
    if (!error.isEmpty()) {
        QMessageBox::warning(parent_widget, title,
                             tr("This NX archive cannot be read: %1").arg(error));
        return;
    }

    // What the game list shows now.
    std::string current_title;
    std::string current_publisher;
    std::vector<u8> current_icon;
    std::vector<u8> original_icon;
    if (const auto file = vfs->OpenFile(archive_path, FileSys::OpenMode::Read);
        file != nullptr && QtCommon::system != nullptr) {
        Loader::AppLoader_NX loader(*QtCommon::system, file);
        loader.ReadTitle(current_title);
        loader.ReadIcon(current_icon);
        loader.ReadPublisher(current_publisher);
        loader.ReadOriginalIcon(application_id.toULongLong(nullptr, 16), original_icon);
    }

    QDialog dialog(parent_widget);
    dialog.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    // Start from the name the game list shows: the title, or the file name without one.
    const QString shown_title =
        current_title.empty() ? QFileInfo(QString::fromStdString(archive_path)).completeBaseName()
                              : QString::fromStdString(current_title);
    auto* name_edit = new QLineEdit(shown_title, &dialog);
    const auto shown_publisher = QString::fromStdString(current_publisher);
    auto* publisher_edit = new QLineEdit(shown_publisher, &dialog);
    form->addRow(tr("Title:"), name_edit);
    form->addRow(tr("Publisher:"), publisher_edit);

    // Drop an image on the icon or click it, as when converting to NUP.
    auto* icon_preview = new IconDropZone(128, &dialog);
    QPixmap shown;
    if (shown.loadFromData(current_icon.data(), static_cast<uint>(current_icon.size()))) {
        icon_preview->SetIcon(shown);
    }
    auto* choose = new QPushButton(tr("Choose Image..."), &dialog);
    auto* size_box = new QComboBox(&dialog);
    size_box->addItem(tr("256 x 256 (default)"), 256);
    size_box->addItem(tr("512 x 512"), 512);
    size_box->addItem(tr("1024 x 1024"), 1024);
    size_box->setToolTip(tr("Size of the icon stored in the archive"));
    size_box->setEnabled(false);
    auto* icon_buttons = new QVBoxLayout;
    icon_buttons->addWidget(choose);
    icon_buttons->addWidget(size_box);
    auto* icon_row = new QHBoxLayout;
    icon_row->addWidget(icon_preview, 0, Qt::AlignTop);
    icon_row->addLayout(icon_buttons);
    QByteArray new_icon;
    // The chosen picture and the square kept from it, encoded at the chosen size.
    QImage source;
    QRect crop;
    const auto encode = [&] {
        if (source.isNull()) {
            return;
        }
        new_icon = EncodeIcon(source, crop, size_box->currentData().toInt());
        if (new_icon.isEmpty()) {
            QMessageBox::warning(&dialog, title, tr("This image cannot be converted."));
            return;
        }
        icon_preview->SetIcon(QPixmap::fromImage(source.copy(crop)));
    };
    const auto use_image = [&](const QString& path) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QImage image = reader.read();
        if (image.isNull()) {
            QMessageBox::warning(&dialog, title, tr("This image cannot be read."));
            return;
        }
        const auto selection = ChooseCrop(image, &dialog);
        if (!selection) {
            return;
        }
        source = image;
        crop = *selection;
        size_box->setEnabled(true);
        encode();
    };
    const auto browse = [&] {
        const QString path = QFileDialog::getOpenFileName(
            &dialog, tr("Choose Image"), {}, tr("Images (*.png *.jpg *.jpeg *.bmp *.webp *.gif)"));
        if (!path.isEmpty()) {
            use_image(path);
        }
    };
    connect(choose, &QPushButton::clicked, &dialog, browse);
    connect(icon_preview, &IconDropZone::Clicked, &dialog, browse);
    connect(icon_preview, &IconDropZone::FileDropped, &dialog, use_image);
    connect(size_box, &QComboBox::currentIndexChanged, &dialog, encode);
    QPixmap original;
    if (!original_icon.empty() &&
        (original.loadFromData(original_icon.data(), static_cast<uint>(original_icon.size()),
                               "JPEG") ||
         original.loadFromData(original_icon.data(), static_cast<uint>(original_icon.size()),
                               "PNG"))) {
        auto* restore = new QPushButton(tr("Restore Original Image"), &dialog);
        icon_buttons->addWidget(restore);
        connect(restore, &QPushButton::clicked, &dialog, [&] {
            source = {};
            size_box->setEnabled(false);
            new_icon = original_icon == current_icon
                           ? QByteArray{}
                           : QByteArray(reinterpret_cast<const char*>(original_icon.data()),
                                        static_cast<qsizetype>(original_icon.size()));
            icon_preview->SetIcon(original);
        });
    }
    icon_buttons->addStretch();
    icon_row->addStretch();
    form->addRow(tr("Icon:"), icon_row);
    layout->addLayout(form);
    layout->addWidget(
        new QLabel(tr("The new title and icon are shown in every language. The game itself is not "
                      "changed."),
                   &dialog));

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(520, 0);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    auto job = std::make_shared<Job>();
    job->title = tr("Editing NX metadata");
    job->args = {"set-metadata", archive_path, ToUtf8(application_id)};
    const auto name = name_edit->text().trimmed();
    if (FrontendCommon::NxArchive::IsTitleEdit(ToUtf8(name), ToUtf8(shown_title.trimmed()))) {
        job->args.push_back("title=" + name.toStdString());
    }
    const auto publisher = publisher_edit->text().trimmed();
    if (!publisher.isEmpty() && publisher != shown_publisher.trimmed()) {
        job->args.push_back("publisher=" + publisher.toStdString());
    }
    if (!new_icon.isEmpty()) {
        job->icon_file = std::make_unique<QTemporaryFile>(QDir::tempPath() +
                                                          QStringLiteral("/eglantine-icon-XXXXXX"));
        if (!job->icon_file->open() || job->icon_file->write(new_icon) != new_icon.size() ||
            !job->icon_file->flush()) {
            QMessageBox::warning(parent_widget, title, tr("The icon cannot be saved."));
            return;
        }
        job->icon_file->close();
        job->args.push_back("icon=" + ToUtf8(job->icon_file->fileName()));
    }
    if (job->args.size() == 3 || running || !prepare_write()) {
        return;
    }
    job->done_message = tr("The NX archive metadata was updated.");
    Start(std::move(job));
}

void NxTools::Start(std::shared_ptr<Job> job) {
    running = job;
    emit RunningChanged(true);

    progress = new QProgressDialog(job->title, tr("Cancel"), 0, 0, parent_widget);
    progress->setWindowTitle(job->title);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    connect(progress, &QProgressDialog::canceled, this, [this, job] {
        nxformat_cancel_set(job->cancel);
        if (progress) {
            progress->setLabelText(tr("Cancelling..."));
        }
    });
    job->clock.start();
    auto* timer = new QTimer(progress);
    connect(timer, &QTimer::timeout, this, [this, job] {
        if (!progress || progress->wasCanceled()) {
            return;
        }
        // nxformat counts the bytes it reads, not the checks it makes before and after:
        // those steps are shown as preparing and finishing, with the time elapsed.
        const auto total = job->total.load();
        const auto done = std::min(job->written.load(), total);
        const qint64 now = job->clock.elapsed();
        QString step;
        if (total == 0) {
            step = tr("Preparing...");
        } else if (done >= total) {
            step = tr("Finishing...");
        } else {
            if (job->counting_since < 0) {
                job->counting_since = now;
            }
            // Time left: measured from the first counted byte, smoothed so it does not jump.
            const double fraction = static_cast<double>(done) / static_cast<double>(total);
            const double counting = (now - job->counting_since) / 1000.0;
            if (fraction >= 0.01 && counting >= 5.0) {
                const double estimate = counting * (1.0 - fraction) / fraction;
                job->seconds_left =
                    job->seconds_left < 0 ? estimate : job->seconds_left * 0.9 + estimate * 0.1;
                step = tr("%1 left").arg(TimeLeft(job->seconds_left));
            }
        }
        const qint64 seconds = now / 1000;
        const QString elapsed = seconds >= 3600
                                    ? QStringLiteral("%1:%2:%3")
                                          .arg(seconds / 3600)
                                          .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
                                          .arg(seconds % 60, 2, 10, QLatin1Char('0'))
                                    : QStringLiteral("%1:%2")
                                          .arg(seconds / 60)
                                          .arg(seconds % 60, 2, 10, QLatin1Char('0'));
        QString text = tr("%1\nElapsed: %2").arg(job->title, elapsed);
        if (!step.isEmpty()) {
            text += QStringLiteral(" - ") + step;
        }
        progress->setLabelText(text);
        if (total == 0) {
            return;
        }
        // setValue() of a modal dialog processes events: Finished() may run inside it and
        // close the dialog, so it must be the last use of `progress` here. It stays below
        // 100 % until nxformat has returned.
        progress->setMaximum(1000);
        progress->setValue(static_cast<int>(std::min<u64>(999, done * 1000 / total)));
    });
    timer->start(200);
    progress->show();

    job->future = QtConcurrent::run([this, job] {
        std::vector<const char*> args;
        args.reserve(job->args.size());
        for (const auto& arg : job->args) {
            args.push_back(arg.c_str());
        }
        char error[1024]{};
        const auto code = nxformat_run(
            args.data(), args.size(),
            [](void* user, int is_error, const char* line) {
                auto& j = *static_cast<Job*>(user);
                if (is_error) {
                    return;
                }
                const auto fields = QString::fromUtf8(line).split(QLatin1Char('\t'));
                if (fields[0] == QStringLiteral("@TOTAL") && fields.size() >= 2) {
                    j.total = j.total.load() + fields[1].toULongLong();
                } else if (fields[0] == QStringLiteral("@PROGRESS") && fields.size() >= 2) {
                    j.written = fields[1].toULongLong();
                } else if (fields[0] == QStringLiteral("@ADDED") && fields.size() >= 4) {
                    std::scoped_lock lock{j.lines_mutex};
                    j.added.push_back(fields[1] + QLatin1Char(' ') + fields[2].toUpper() +
                                      QStringLiteral(" v") + fields[3]);
                } else if (fields[0] == QStringLiteral("@SKIPPED") && fields.size() >= 4) {
                    std::scoped_lock lock{j.lines_mutex};
                    auto entry = fields[1] + QLatin1Char(' ') + fields[2].toUpper() +
                                 QStringLiteral(" v") + fields[3];
                    // A retry finds what the failed run added: it is listed as added.
                    if (!j.added.contains(entry)) {
                        j.skipped.push_back(std::move(entry));
                    }
                }
            },
            job.get(), job->cancel, error, sizeof(error));
        QMetaObject::invokeMethod(
            this,
            [this, job, code, message = QString::fromUtf8(error)] { Finished(job, code, message); },
            Qt::QueuedConnection);
    });
}

void NxTools::ReplyOriginal(Job& job, const QString& text) {
    const auto& originals = job.originals;
    QStringList names;
    for (const auto& original : originals) {
        names.push_back(ToQString(original));
    }
    QString label;
    if (originals.size() > 1) {
        label = tr("Move the %1 original packages to the Recycle Bin").arg(originals.size());
    } else if (std::error_code ec; std::filesystem::is_directory(originals.front(), ec)) {
        label = tr("Move the original folder to the Recycle Bin:\n%1").arg(names.front());
    } else {
        label = tr("Move the original game to the Recycle Bin:\n%1").arg(names.front());
    }
    QMessageBox box(QMessageBox::Information, job.title, text, QMessageBox::Ok, parent_widget);
    auto* remove = new QCheckBox(label, &box);
    box.setCheckBox(remove);
    box.exec();
    if (remove->isChecked()) {
        // nxformat has returned; close what it read before moving the originals.
        job.ReleaseSources();
        // The game list's content provider keeps every package it scanned open, for updates and
        // DLC. Drop it (the refresh below fills it again), unless a game is running.
        if (QtCommon::provider != nullptr && !QtCommon::system->IsPoweredOn()) {
            QtCommon::provider->ClearAllEntries();
        }
        int failed = 0;
        for (const auto& name : names) {
            if (!QFile::moveToTrash(name)) {
                ++failed;
            }
        }
        if (failed > 0) {
            QMessageBox::warning(
                parent_widget, job.title,
                originals.size() > 1
                    ? tr("%1 of the %2 original packages could not be moved to the Recycle Bin. "
                         "They may be in use.")
                          .arg(failed)
                          .arg(originals.size())
                    : tr("The original could not be moved to the Recycle Bin:\n%1\n\n"
                         "It may be in use. You can delete it yourself.")
                          .arg(names.front()));
        }
    }
    emit ArchivesChanged();
}

QString NxTools::TimeLeft(double seconds) {
    const auto minutes = static_cast<qint64>(seconds / 60.0 + 0.5);
    if (seconds < 60.0) {
        return tr("less than a minute");
    }
    if (minutes < 60) {
        return tr("about %n minute(s)", nullptr, static_cast<int>(minutes));
    }
    return tr("about %1 h %2 min").arg(minutes / 60).arg(minutes % 60, 2, 10, QLatin1Char('0'));
}

void NxTools::Finished(std::shared_ptr<Job> job, int code, QString error) {
    if (progress) {
        // Closing a QProgressDialog emits canceled(): disconnect first, or a failed or finished
        // job shows "Cancelling..." behind its result.
        QObject::disconnect(progress, nullptr, this, nullptr);
        progress->hide();
        progress->deleteLater();
        progress.clear();
    }
    running.reset();
    emit RunningChanged(false);
    const bool metadata_write = job->args.front() == "set-metadata";
    if (metadata_write) {
        emit MetadataWriteFinished();
    }

    // add keeps the content it committed before a failure or a cancel, also across a retry:
    // report it and refresh.
    const bool changed = !job->added.isEmpty();
    const QString kept =
        changed
            ? QStringLiteral("\n\n") +
                  tr("Already added to the archive: %1").arg(job->added.join(QStringLiteral(", ")))
            : QString{};
    switch (code) {
    case NXFORMAT_OK: {
        QString text = job->done_message;
        if (!job->originals.empty()) {
            ReplyOriginal(*job, text);
            break;
        }
        if (!job->added.isEmpty()) {
            text +=
                QStringLiteral("\n\n") + tr("Added: %1").arg(job->added.join(QStringLiteral(", ")));
        }
        if (!job->skipped.isEmpty()) {
            text += QStringLiteral("\n\n") +
                    tr("Already in the archive: %1").arg(job->skipped.join(QStringLiteral(", ")));
        }
        QMessageBox::information(parent_widget, job->title, text);
        if (!metadata_write) {
            emit ArchivesChanged();
        }
        break;
    }
    case NXFORMAT_CANCELLED:
        if (changed) {
            QMessageBox::information(parent_widget, job->title, tr("Cancelled.") + kept);
            emit ArchivesChanged();
        } else {
            QMessageBox::information(parent_widget, job->title,
                                     tr("Cancelled. Nothing was changed on disk."));
        }
        break;
    default: {
        if (job->CanRetryWithoutRecipe(error)) {
            const auto answer = QMessageBox::question(
                parent_widget, job->title,
                tr("The operation failed because the rebuild recipe could not be kept:\n%1\n\n"
                   "Try again without the recipe? The game still runs from the NX archive, but "
                   "the original package cannot be rebuilt exactly from it.")
                        .arg(error) +
                    kept,
                QMessageBox::Retry | QMessageBox::Cancel, QMessageBox::Retry);
            if (answer == QMessageBox::Retry) {
                // The retry refreshes the archives when it ends.
                job->PrepareRetryWithoutRecipe();
                Start(std::move(job));
                break;
            }
        } else {
            QMessageBox::warning(parent_widget, job->title,
                                 tr("The operation failed:\n%1").arg(error) + kept);
        }
        if (changed) {
            emit ArchivesChanged();
        }
        break;
    }
    }
}
