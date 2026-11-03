// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/nup_dialogs.h"

#include <QBuffer>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QStyle>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

#include "common/fs/path_util.h"
#include "yuzu/util/icon_search.h"

namespace {

constexpr int IconSize = 256;
constexpr int PreviewSize = 128;

QString ToQString(const std::filesystem::path& path) {
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}

QString Megabytes(u64 bytes) {
    return QString::number(bytes / (1024 * 1024));
}

QPixmap PixmapFromBytes(const std::vector<u8>& bytes) {
    QPixmap pixmap;
    pixmap.loadFromData(bytes.data(), static_cast<uint>(bytes.size()));
    return pixmap;
}

QPixmap RoundedPreview(const QPixmap& source, int size) {
    const qreal ratio = 2.0;
    QPixmap out(QSize(size, size) * ratio);
    out.setDevicePixelRatio(ratio);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, size, size), size / 8.0, size / 8.0);
    painter.setClipPath(clip);
    painter.drawPixmap(QRectF(0, 0, size, size), source, source.rect());
    return out;
}

// The console distributes 256x256 JPEG icons. A picture that already is one is kept as is;
// anything else is cropped to a square, scaled and re-encoded.
QByteArray EncodeIconData(const QByteArray& raw) {
    QImage image;
    if (!image.loadFromData(raw) || image.isNull()) {
        return {};
    }
    if (image.size() == QSize(IconSize, IconSize) && raw.startsWith("\xFF\xD8")) {
        return raw;
    }
    const int side = std::min(image.width(), image.height());
    image = image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side)
                .scaled(IconSize, IconSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage flat(IconSize, IconSize, QImage::Format_RGB32);
    flat.fill(Qt::black);
    QPainter painter(&flat);
    painter.drawImage(0, 0, image);
    painter.end();
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    flat.save(&buffer, "JPEG", 92);
    return out;
}

QByteArray EncodeIcon(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return EncodeIconData(file.readAll());
}

struct LocalizedLanguage {
    const char* tag;
    const char* name;
};

// The languages the console stores a name for; "en-us" is the Name field itself, and tags not
// listed here are kept as stored.
constexpr std::array<LocalizedLanguage, 15> LocalizedLanguages{{
    {"en-gb", QT_TRANSLATE_NOOP("NupEditDialog", "English (UK)")},
    {"ja", QT_TRANSLATE_NOOP("NupEditDialog", "Japanese")},
    {"fr", QT_TRANSLATE_NOOP("NupEditDialog", "French")},
    {"de", QT_TRANSLATE_NOOP("NupEditDialog", "German")},
    {"es-419", QT_TRANSLATE_NOOP("NupEditDialog", "Spanish (Latin America)")},
    {"es", QT_TRANSLATE_NOOP("NupEditDialog", "Spanish")},
    {"it", QT_TRANSLATE_NOOP("NupEditDialog", "Italian")},
    {"nl", QT_TRANSLATE_NOOP("NupEditDialog", "Dutch")},
    {"fr-ca", QT_TRANSLATE_NOOP("NupEditDialog", "French (Canada)")},
    {"pt", QT_TRANSLATE_NOOP("NupEditDialog", "Portuguese")},
    {"ru", QT_TRANSLATE_NOOP("NupEditDialog", "Russian")},
    {"ko", QT_TRANSLATE_NOOP("NupEditDialog", "Korean")},
    {"zh-hant", QT_TRANSLATE_NOOP("NupEditDialog", "Chinese (Traditional)")},
    {"zh-hans", QT_TRANSLATE_NOOP("NupEditDialog", "Chinese (Simplified)")},
    {"pt-br", QT_TRANSLATE_NOOP("NupEditDialog", "Portuguese (Brazil)")},
}};

bool IsEditableLanguage(const std::string& tag) {
    return std::ranges::any_of(LocalizedLanguages,
                               [&](const auto& language) { return tag == language.tag; });
}

// The first dropped local file that Qt can read as an image.
QString DroppedImage(const QMimeData* mime) {
    if (mime == nullptr) {
        return {};
    }
    for (const auto& url : mime->urls()) {
        if (url.isLocalFile() && !QImageReader(url.toLocalFile()).format().isEmpty()) {
            return url.toLocalFile();
        }
    }
    return {};
}

} // namespace

IconDropZone::IconDropZone(int size, QWidget* parent) : QWidget(parent) {
    setFixedSize(size, size);
    setAcceptDrops(true);
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Drop an image here or click to browse"));
}

void IconDropZone::SetIcon(const QPixmap& new_icon) {
    icon = new_icon;
    update();
}

void IconDropZone::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF area = QRectF(rect()).adjusted(1, 1, -1, -1);
    const bool active = dragging || hovered;
    if (!icon.isNull()) {
        painter.drawPixmap(0, 0, RoundedPreview(icon, width()));
        if (active) {
            painter.setBrush(QColor(0, 0, 0, 120));
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(area, width() / 8.0, width() / 8.0);
            painter.setPen(Qt::white);
            painter.drawText(area, Qt::AlignCenter | Qt::TextWordWrap, tr("Drop to replace"));
        }
        return;
    }
    const auto accent = palette().color(QPalette::Highlight);
    QPen pen(active ? accent : palette().color(QPalette::Mid), 2, Qt::DashLine);
    painter.setPen(pen);
    QColor fill = active ? accent : palette().color(QPalette::Mid);
    fill.setAlpha(active ? 40 : 20);
    painter.setBrush(fill);
    painter.drawRoundedRect(area, width() / 8.0, width() / 8.0);
    painter.setPen(palette().color(QPalette::PlaceholderText));
    painter.drawText(area.adjusted(8, 8, -8, -8), Qt::AlignCenter | Qt::TextWordWrap,
                     dragging ? tr("Release to use this image")
                              : tr("Drop an icon here\nor click to browse"));
}

void IconDropZone::dragEnterEvent(QDragEnterEvent* event) {
    if (!DroppedImage(event->mimeData()).isEmpty()) {
        dragging = true;
        event->acceptProposedAction();
        update();
    }
}

void IconDropZone::dragLeaveEvent(QDragLeaveEvent*) {
    dragging = false;
    update();
}

void IconDropZone::dropEvent(QDropEvent* event) {
    dragging = false;
    update();
    if (const auto path = DroppedImage(event->mimeData()); !path.isEmpty()) {
        event->acceptProposedAction();
        emit FileDropped(path);
    }
}

void IconDropZone::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
        emit Clicked();
    }
}

void IconDropZone::enterEvent(QEnterEvent*) {
    hovered = true;
    update();
}

void IconDropZone::leaveEvent(QEvent*) {
    hovered = false;
    update();
}

NupMetadataDialog::NupMetadataDialog(const std::filesystem::path& source,
                                     const std::filesystem::path& archive, u64 needed_bytes,
                                     u64 available_bytes, QWidget* parent)
    : QDialog(parent), detected(FileSys::Nup::ProbeMetadata(source)) {
    setWindowTitle(tr("Convert to NUP"));
    setMinimumWidth(520);

    icon_preview = new IconDropZone(PreviewSize);
    connect(icon_preview, &IconDropZone::Clicked, this, &NupMetadataDialog::ChooseIcon);
    connect(icon_preview, &IconDropZone::FileDropped, this, &NupMetadataDialog::SetIconFromFile);
    icon_hint = new QLabel;
    icon_hint->setWordWrap(true);
    icon_hint->setEnabled(false);
    auto* choose_icon = new QPushButton(tr("Choose Icon..."));
    reset_icon = new QPushButton(tr("Use Detected"));
    connect(choose_icon, &QPushButton::clicked, this, &NupMetadataDialog::ChooseIcon);
    connect(reset_icon, &QPushButton::clicked, this, &NupMetadataDialog::ResetIcon);

    auto* icon_buttons = new QVBoxLayout;
    icon_buttons->addWidget(choose_icon);
    if (!detected.icon.empty()) {
        icon_buttons->addWidget(reset_icon);
    } else {
        reset_icon->hide();
    }
    icon_buttons->addWidget(icon_hint);
    icon_buttons->addStretch();
    auto* icon_row = new QHBoxLayout;
    icon_row->setSpacing(16);
    icon_row->addWidget(icon_preview, 0, Qt::AlignTop);
    icon_row->addLayout(icon_buttons, 1);

    name_edit = new QLineEdit(QString::fromStdString(detected.title_name));
    name_edit->setPlaceholderText(tr("Name shown in the game list"));
    name_edit->setClearButtonEnabled(true);

    publisher_edit = new QLineEdit(QString::fromStdString(detected.publisher));
    publisher_edit->setPlaceholderText(tr("Publisher shown in the game list"));
    publisher_edit->setClearButtonEnabled(true);

    version_auto = new QRadioButton;
    if (detected.display_version.empty()) {
        version_auto->setText(tr("Auto-detect (nothing found in the dump)"));
    } else {
        version_auto->setText(tr("Auto-detect: %1 (from %2)")
                                  .arg(QString::fromStdString(detected.display_version),
                                       QString::fromStdString(detected.version_source)));
    }
    version_manual = new QRadioButton(tr("Set manually:"));
    version_edit = new QLineEdit;
    version_edit->setPlaceholderText(tr("e.g. 1.2.0"));
    version_auto->setChecked(true);
    connect(version_auto, &QRadioButton::toggled, this, &NupMetadataDialog::UpdateVersionEnabled);
    auto* manual_row = new QHBoxLayout;
    manual_row->addWidget(version_manual);
    manual_row->addWidget(version_edit, 1);
    auto* version_box = new QVBoxLayout;
    version_box->setSpacing(6);
    version_box->addWidget(version_auto);
    version_box->addLayout(manual_row);

    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(14);
    form->addRow(tr("Icon"), icon_row);
    form->addRow(tr("Name"), name_edit);
    form->addRow(tr("Publisher"), publisher_edit);
    form->addRow(tr("Version"), version_box);

    auto* paths = new QLabel(tr("<b>Source</b><br>%1<br><b>Target</b><br>%2")
                                 .arg(ToQString(source).toHtmlEscaped(),
                                      ToQString(archive).toHtmlEscaped()));
    paths->setTextFormat(Qt::RichText);
    paths->setWordWrap(true);
    paths->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* space = new QLabel(tr("Approximate space needed: %1 MB. Free on the target volume: "
                                "%2 MB.")
                                 .arg(Megabytes(needed_bytes), Megabytes(available_bytes)));
    space->setWordWrap(true);
    if (available_bytes < needed_bytes) {
        space->setText(space->text() + QLatin1Char(' ') +
                       tr("Free space is less than the estimate."));
    }

    low_storage = new QCheckBox(tr("Low storage: delete the original files as they are packed "
                                   "(cannot be undone)"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto* convert = buttons->addButton(tr("Convert"), QDialogButtonBox::AcceptRole);
    convert->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* separator = new QFrame;
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 16);
    layout->setSpacing(14);
    layout->addLayout(form);
    layout->addWidget(separator);
    layout->addWidget(paths);
    layout->addWidget(space);
    layout->addWidget(low_storage);
    layout->addSpacing(4);
    layout->addWidget(buttons);

    UpdateIconPreview();
    UpdateVersionEnabled();
}

NupMetadataDialog::~NupMetadataDialog() = default;

FileSys::Nup::Metadata NupMetadataDialog::Metadata() const {
    FileSys::Nup::Metadata result;
    // Only what differs from the dump is an override.
    if (const auto name = name_edit->text().trimmed().toStdString(); name != detected.title_name) {
        result.title_name = name;
    }
    if (const auto publisher = publisher_edit->text().trimmed().toStdString();
        publisher != detected.publisher) {
        result.publisher = publisher;
    }
    if (!custom_icon.isEmpty()) {
        result.icon.assign(custom_icon.begin(), custom_icon.end());
    }
    if (version_manual->isChecked()) {
        result.display_version = version_edit->text().trimmed().toStdString();
    }
    return result;
}

bool NupMetadataDialog::DeleteSources() const {
    return low_storage->isChecked();
}

QString NupMetadataDialog::DisplayName() const {
    return name_edit->text().trimmed();
}

QPixmap NupMetadataDialog::CurrentIcon() const {
    return current_icon;
}

void NupMetadataDialog::ChooseIcon() {
    const auto path = QFileDialog::getOpenFileName(
        this, tr("Choose Icon"), {}, tr("Images (*.png *.jpg *.jpeg *.bmp *.webp);;All Files (*)"));
    if (!path.isEmpty()) {
        SetIconFromFile(path);
    }
}

void NupMetadataDialog::SetIconFromFile(const QString& path) {
    const auto encoded = EncodeIcon(path);
    if (encoded.isEmpty()) {
        icon_hint->setText(tr("That file could not be read as an image."));
        return;
    }
    custom_icon = encoded;
    UpdateIconPreview();
}

void NupMetadataDialog::ResetIcon() {
    custom_icon.clear();
    UpdateIconPreview();
}

void NupMetadataDialog::UpdateIconPreview() {
    QPixmap pixmap;
    if (!custom_icon.isEmpty()) {
        pixmap.loadFromData(custom_icon);
        icon_hint->setText(tr("Custom icon, stored as %1x%1 JPEG.").arg(IconSize));
    } else if (!detected.icon.empty()) {
        pixmap = PixmapFromBytes(detected.icon);
        icon_hint->setText(tr("Icon found in the dump."));
    } else {
        icon_hint->setText(tr("No icon found in the dump. Add one, or the NUP will have none."));
    }
    current_icon = pixmap;
    reset_icon->setEnabled(!custom_icon.isEmpty());
    icon_preview->SetIcon(pixmap);
}

void NupMetadataDialog::UpdateVersionEnabled() {
    version_edit->setEnabled(version_manual->isChecked());
    if (version_manual->isChecked()) {
        version_edit->setFocus();
    }
}

NupProgressDialog::NupProgressDialog(const QString& operation, const QString& title,
                                       const QPixmap& icon, QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint) {
    setWindowTitle(operation);
    setModal(true);
    setMinimumWidth(480);

    auto* header = new QHBoxLayout;
    header->setSpacing(14);
    if (!icon.isNull()) {
        auto* icon_label = new QLabel;
        icon_label->setFixedSize(56, 56);
        icon_label->setPixmap(RoundedPreview(icon, 56));
        header->addWidget(icon_label);
    }
    auto* title_label = new QLabel(title.isEmpty() ? operation : title);
    QFont font = title_label->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * 1.2);
    title_label->setFont(font);
    title_label->setWordWrap(true);
    status = new QLabel(tr("Preparing..."));
    status->setEnabled(false);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addStretch();
    titles->addWidget(title_label);
    titles->addWidget(status);
    titles->addStretch();
    header->addLayout(titles, 1);

    bar = new QProgressBar;
    bar->setRange(0, 1000);
    bar->setTextVisible(false);
    bar->setFixedHeight(14);
    bar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    details = new QLabel;
    details->setAlignment(Qt::AlignCenter);
    details->setMinimumHeight(QFontMetrics(details->font()).height() * 2 + 4);

    cancel = new QPushButton(tr("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &NupProgressDialog::CancelRequested);
    auto* button_row = new QHBoxLayout;
    button_row->addStretch();
    button_row->addWidget(cancel);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(14);
    layout->addLayout(header);
    layout->addWidget(bar);
    layout->addWidget(details);
    layout->addLayout(button_row);
    timer.start();
}

void NupProgressDialog::SetProgress(const FileSys::Nup::PackProgress& progress) {
    if (cancelling) {
        return;
    }
    const double fraction =
        progress.bytes_total == 0 ? 0.0 : double(progress.bytes_done) / progress.bytes_total;
    bar->setValue(static_cast<int>(fraction * 1000));
    status->setText(tr("%1%").arg(fraction * 100, 0, 'f', 1));

    QString text = tr("%1 / %2 MB  -  %3 / %4 files")
                       .arg(Megabytes(progress.bytes_done), Megabytes(progress.bytes_total))
                       .arg(progress.files_done)
                       .arg(progress.files_total);
    const double seconds = timer.elapsed() / 1000.0;
    if (seconds >= 3.0 && progress.bytes_done > 0 && fraction < 1.0) {
        const double rate = progress.bytes_done / seconds;
        const int remaining = static_cast<int>((progress.bytes_total - progress.bytes_done) / rate);
        text += tr("\n%1 MB/s  -  about %2:%3 left")
                    .arg(rate / (1024 * 1024), 0, 'f', 0)
                    .arg(remaining / 60)
                    .arg(remaining % 60, 2, 10, QLatin1Char('0'));
    }
    details->setText(text);
}

void NupProgressDialog::SetCancelling() {
    cancelling = true;
    cancel->setEnabled(false);
    status->setText(tr("Cancelling..."));
    bar->setRange(0, 0);
}

void NupProgressDialog::Finish() {
    finished = true;
    close();
}

void NupProgressDialog::closeEvent(QCloseEvent* event) {
    // Only the window code closes this dialog; a close request cancels the conversion.
    if (!finished) {
        emit CancelRequested();
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void NupProgressDialog::reject() {
    if (!finished) {
        emit CancelRequested();
        return;
    }
    QDialog::reject();
}

NupEditDialog::NupEditDialog(const std::filesystem::path& archive, QWidget* parent)
    : QDialog(parent), current(FileSys::Nup::ProbeNupMetadata(archive)) {
    setWindowTitle(tr("Edit NUP Metadata"));
    setMinimumWidth(520);

    icon_preview = new IconDropZone(PreviewSize);
    connect(icon_preview, &IconDropZone::Clicked, this, &NupEditDialog::ChooseIcon);
    connect(icon_preview, &IconDropZone::FileDropped, this, &NupEditDialog::SetIconFromFile);
    icon_hint = new QLabel;
    icon_hint->setWordWrap(true);
    icon_hint->setEnabled(false);
    auto* choose_icon = new QPushButton(tr("Choose Icon..."));
    find_icon = new QPushButton(tr("Find Icon Online..."));
    clear_icon = new QPushButton(tr("Remove Icon"));
    icon_search = new IconSearch(this);
    connect(choose_icon, &QPushButton::clicked, this, &NupEditDialog::ChooseIcon);
    connect(find_icon, &QPushButton::clicked, this, &NupEditDialog::FindIcon);
    connect(icon_search, &IconSearch::RunningChanged, this, [this](bool running) {
        find_icon->setEnabled(!running);
        find_icon->setText(running ? tr("Searching...") : tr("Find Icon Online..."));
    });
    connect(icon_search, &IconSearch::IconSelected, this, &NupEditDialog::OnIconFound);
    connect(clear_icon, &QPushButton::clicked, this, &NupEditDialog::ClearIcon);

    auto* icon_buttons = new QVBoxLayout;
    icon_buttons->addWidget(choose_icon);
    icon_buttons->addWidget(find_icon);
    icon_buttons->addWidget(clear_icon);
    icon_buttons->addWidget(icon_hint);
    icon_buttons->addStretch();
    auto* icon_row = new QHBoxLayout;
    icon_row->setSpacing(16);
    icon_row->addWidget(icon_preview, 0, Qt::AlignTop);
    icon_row->addLayout(icon_buttons, 1);

    name_edit = new QLineEdit(QString::fromStdString(current.title_name));
    name_edit->setPlaceholderText(tr("Name shown in the game list"));
    name_edit->setClearButtonEnabled(true);

    publisher_edit = new QLineEdit(QString::fromStdString(current.publisher));
    publisher_edit->setPlaceholderText(tr("Publisher shown in the game list"));
    publisher_edit->setClearButtonEnabled(true);

    version_edit = new QLineEdit(QString::fromStdString(current.display_version));
    version_edit->setPlaceholderText(tr("e.g. 1.2.0"));
    version_edit->setClearButtonEnabled(true);

    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(14);
    form->addRow(tr("Icon"), icon_row);
    localized_toggle = new QToolButton;
    localized_toggle->setCheckable(true);
    localized_toggle->setAutoRaise(true);
    localized_toggle->setIcon(QIcon::fromTheme(
        QStringLiteral("preferences-desktop-locale"),
        QIcon::fromTheme(QStringLiteral("applications-education-language"),
                         style()->standardIcon(QStyle::SP_FileDialogDetailedView))));
    auto* name_row = new QHBoxLayout;
    name_row->setSpacing(6);
    name_row->addWidget(name_edit, 1);
    name_row->addWidget(localized_toggle);

    localized_panel = new QWidget;
    auto* localized_form = new QFormLayout(localized_panel);
    localized_form->setContentsMargins(0, 0, 0, 0);
    localized_form->setVerticalSpacing(8);
    int localized_set = 0;
    for (const auto& language : LocalizedLanguages) {
        auto* field = new QLineEdit;
        field->setClearButtonEnabled(true);
        for (const auto& [tag, name] : current.localized_titles) {
            if (tag == language.tag) {
                field->setText(QString::fromStdString(name));
                ++localized_set;
            }
        }
        localized_form->addRow(tr(language.name), field);
        localized_edits.emplace_back(language.tag, field);
    }
    const auto update_placeholders = [this] {
        const QString fallback = name_edit->text().trimmed();
        for (const auto& [tag, field] : localized_edits) {
            field->setPlaceholderText(fallback.isEmpty() ? tr("Same as Name") : fallback);
        }
    };
    connect(name_edit, &QLineEdit::textChanged, this, update_placeholders);
    update_placeholders();
    localized_panel->setVisible(false);
    connect(localized_toggle, &QToolButton::toggled, this, [this](bool shown) {
        localized_panel->setVisible(shown);
        // Recompute the layout first, otherwise the old height is kept and the extra space ends
        // up between the rows.
        layout()->activate();
        resize(width(), minimumSizeHint().height());
    });
    localized_toggle->setToolTip(localized_set > 0
                                     ? tr("Localized names (%1 set)").arg(localized_set)
                                     : tr("Localized names"));

    form->addRow(tr("Name"), name_row);
    form->addRow(QString{}, localized_panel);
    form->addRow(tr("Publisher"), publisher_edit);
    form->addRow(tr("Version"), version_edit);

    auto* path_label = new QLabel(tr("<b>File</b><br>%1").arg(ToQString(archive).toHtmlEscaped()));
    path_label->setTextFormat(Qt::RichText);
    path_label->setWordWrap(true);
    path_label->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* hint = new QLabel(tr("Empty fields remove that metadata. The change is written in place;"
                               " game data is untouched."));
    hint->setWordWrap(true);
    hint->setEnabled(false);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 16);
    layout->setSpacing(14);
    layout->addLayout(form);
    layout->addWidget(path_label);
    layout->addWidget(hint);
    layout->addWidget(buttons);

    UpdateIconPreview();
}

NupEditDialog::~NupEditDialog() = default;

FileSys::Nup::MetadataEdit NupEditDialog::Edit() const {
    FileSys::Nup::MetadataEdit edit;
    if (remove_icon) {
        edit.icon = std::vector<u8>{};
    } else if (!custom_icon.isEmpty()) {
        edit.icon = std::vector<u8>(custom_icon.begin(), custom_icon.end());
    }
    if (const auto name = name_edit->text().trimmed().toStdString(); name != current.title_name) {
        edit.title_name = name;
    }
    // Tags the console has no field for stay as stored.
    std::vector<std::pair<std::string, std::string>> localized;
    for (const auto& entry : current.localized_titles) {
        if (!IsEditableLanguage(entry.first)) {
            localized.push_back(entry);
        }
    }
    for (const auto& [tag, field] : localized_edits) {
        if (const auto name = field->text().trimmed().toStdString(); !name.empty()) {
            localized.emplace_back(tag, name);
        }
    }
    auto stored = current.localized_titles;
    auto wanted = localized;
    std::ranges::sort(stored);
    std::ranges::sort(wanted);
    if (stored != wanted) {
        edit.localized_titles = std::move(localized);
    }
    if (const auto publisher = publisher_edit->text().trimmed().toStdString();
        publisher != current.publisher) {
        edit.publisher = publisher;
    }
    if (const auto version = version_edit->text().trimmed().toStdString();
        version != current.display_version) {
        edit.display_version = version;
    }
    return edit;
}

QString NupEditDialog::DisplayName() const {
    return name_edit->text().trimmed();
}

QPixmap NupEditDialog::CurrentIcon() const {
    return current_icon;
}

void NupEditDialog::ChooseIcon() {
    const auto path = QFileDialog::getOpenFileName(
        this, tr("Choose Icon"), {}, tr("Images (*.png *.jpg *.jpeg *.bmp *.webp);;All Files (*)"));
    if (!path.isEmpty()) {
        SetIconFromFile(path);
    }
}

void NupEditDialog::SetIconFromFile(const QString& path) {
    const auto encoded = EncodeIcon(path);
    if (encoded.isEmpty()) {
        icon_hint->setText(tr("That file could not be read as an image."));
        return;
    }
    custom_icon = encoded;
    remove_icon = false;
    UpdateIconPreview();
}

void NupEditDialog::FindIcon() {
    icon_search->Start(name_edit->text());
}

void NupEditDialog::OnIconFound(const QByteArray& png, const QString& resolved_title) {
    const auto encoded = EncodeIconData(png);
    if (encoded.isEmpty()) {
        icon_hint->setText(tr("The icon that was found could not be read."));
        return;
    }
    custom_icon = encoded;
    remove_icon = false;
    if (!resolved_title.isEmpty()) {
        name_edit->setText(resolved_title);
    }
    UpdateIconPreview();
}

void NupEditDialog::ClearIcon() {
    custom_icon.clear();
    remove_icon = true;
    UpdateIconPreview();
}

void NupEditDialog::UpdateIconPreview() {
    QPixmap pixmap;
    if (remove_icon) {
        icon_hint->setText(tr("Icon will be removed."));
    } else if (!custom_icon.isEmpty()) {
        pixmap.loadFromData(custom_icon);
        icon_hint->setText(tr("Custom icon, stored as %1x%1 JPEG.").arg(IconSize));
    } else if (!current.icon.empty()) {
        pixmap = PixmapFromBytes(current.icon);
        icon_hint->setText(tr("Current icon in the NUP."));
    } else {
        icon_hint->setText(tr("No icon in the NUP. Add one, or it will have none."));
    }
    current_icon = pixmap;
    icon_preview->SetIcon(pixmap);
}
