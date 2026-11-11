// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <string>

#include <QObject>
#include <QPointer>

#include "core/file_sys/vfs/vfs_types.h"

class QProgressDialog;
class QWidget;

/// NX archive tools of the game list, run in the process with nxformat:
/// conversion to NX of an NSP, XCI, NUP or loose dump folder, and in-place editing of an
/// NX archive (add an update folder, an NSP or an XCI; remove an update or DLC).
/// NSP and XCI are decrypted by the emulator while nxformat reads them: nxformat never sees
/// keys or encrypted bytes, and nothing decrypted is written to disk.
class NxTools : public QObject {
    Q_OBJECT

public:
    NxTools(FileSys::VirtualFilesystem vfs, QWidget* parent);
    ~NxTools() override;

    /// True when ConvertToNx accepts this game list entry.
    static bool CanConvert(const std::string& game_path);
    /// True when the path is an NX archive.
    static bool IsNxArchive(const std::string& game_path);

    void ConvertToNx(const std::string& game_path);
    void AddToNx(const std::string& archive_path);
    void RemoveFromNx(const std::string& archive_path);
    /// Sets the title, publisher and icon the game list shows, in place.
    void EditNxMetadata(const std::string& archive_path,
                        const std::function<bool()>& prepare_write);

signals:
    /// A conversion or an edit started (true) or ended (false).
    void RunningChanged(bool running);
    /// An archive was created or changed: the game list must be refreshed.
    void ArchivesChanged();
    void MetadataWriteFinished();

private:
    struct Job;
    void Start(std::shared_ptr<Job> job);
    void Finished(std::shared_ptr<Job> job, int code, QString error);
    void ReplyOriginal(Job& job, const QString& text);
    static QString TimeLeft(double seconds);

    FileSys::VirtualFilesystem vfs;
    QWidget* parent_widget;
    std::shared_ptr<Job> running;
    QPointer<QProgressDialog> progress;
};
