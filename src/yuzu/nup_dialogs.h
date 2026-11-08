// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QDialog>
#include <QElapsedTimer>
#include <QPixmap>
#include <QWidget>

#include "common/common_types.h"
#include "core/file_sys/nup_writer.h"

class IconSearch;
class QCheckBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QToolButton;

// Square preview that accepts an image dropped on it or chosen with a click.
class IconDropZone : public QWidget {
    Q_OBJECT
public:
    explicit IconDropZone(int size, QWidget* parent = nullptr);

    void SetIcon(const QPixmap& icon);

signals:
    void FileDropped(const QString& path);
    void Clicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QPixmap icon;
    bool dragging = false;
    bool hovered = false;
};

// Asks for the metadata a NUP is written with and for the conversion options.
class NupMetadataDialog : public QDialog {
    Q_OBJECT
public:
    NupMetadataDialog(const std::filesystem::path& source, const std::filesystem::path& archive,
                      u64 needed_bytes, u64 available_bytes, QWidget* parent = nullptr);
    ~NupMetadataDialog() override;

    // Empty fields keep what the dump supplies.
    FileSys::Nup::Metadata Metadata() const;
    bool DeleteSources() const;
    // What the summary shows: the name entered and the icon that will be stored.
    QString DisplayName() const;
    QPixmap CurrentIcon() const;

private:
    void ChooseIcon();
    void SetIconFromFile(const QString& path);
    void ResetIcon();
    void UpdateIconPreview();
    void UpdateVersionEnabled();

    FileSys::Nup::Metadata detected;
    QByteArray custom_icon;
    QPixmap current_icon;

    IconDropZone* icon_preview;
    QLabel* icon_hint;
    QPushButton* reset_icon;
    QLineEdit* name_edit;
    QLineEdit* publisher_edit;
    QRadioButton* version_auto;
    QRadioButton* version_manual;
    QLineEdit* version_edit;
    QCheckBox* low_storage;
};

// Edits the default-language metadata of an existing NUP in place.
class NupEditDialog : public QDialog {
    Q_OBJECT
public:
    explicit NupEditDialog(const std::filesystem::path& archive, QWidget* parent = nullptr);
    ~NupEditDialog() override;

    FileSys::Nup::MetadataEdit Edit() const;
    QString DisplayName() const;
    QPixmap CurrentIcon() const;

private:
    void ChooseIcon();
    void SetIconFromFile(const QString& path);
    void FindIcon();
    void OnIconFound(const QByteArray& png, const QString& resolved_title);
    void ClearIcon();
    void UpdateIconPreview();

    FileSys::Nup::Metadata current;
    QByteArray custom_icon;
    bool remove_icon = false;
    QPixmap current_icon;

    IconDropZone* icon_preview;
    QLabel* icon_hint;
    QPushButton* find_icon;
    QPushButton* clear_icon;
    IconSearch* icon_search;
    QLineEdit* name_edit;
    QLineEdit* publisher_edit;
    QLineEdit* version_edit;
    QToolButton* localized_toggle;
    QWidget* localized_panel;
    std::vector<std::pair<std::string, QLineEdit*>> localized_edits;
};

// Shown while a conversion runs. The bar spans the dialog; the labels show what is being
// converted, how much is done and a rough time remaining.
class NupProgressDialog : public QDialog {
    Q_OBJECT
public:
    NupProgressDialog(const QString& operation, const QString& title, const QPixmap& icon,
                      QWidget* parent = nullptr);

    void SetProgress(const FileSys::Nup::PackProgress& progress);
    void SetCancelling();
    // Closes the dialog without treating it as a cancel request.
    void Finish();

signals:
    void CancelRequested();

protected:
    void closeEvent(QCloseEvent* event) override;
    void reject() override;

private:
    QLabel* status;
    QLabel* details;
    QProgressBar* bar;
    QPushButton* cancel;
    QElapsedTimer timer;
    bool cancelling = false;
    bool finished = false;
};
