// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include "common/net/net.h"
#include "common/scm_rev.h"
#include "tester_build_settings.h"

namespace {

constexpr int ReleaseIndex = 0;
constexpr int TesterIndex = 1;
constexpr int LookupTimeoutMilliseconds = 5000;

std::optional<TesterBuild::Source> ParseEntered(const QString& text) {
    return TesterBuild::ParseSource(text.trimmed().toStdString(), Common::g_build_auto_update_repo);
}

QString SourceText(const TesterBuild::Source& source) {
    return source.kind == TesterBuild::Source::Kind::Branch
               ? QString::fromStdString(source.branch)
               : QStringLiteral("#%1").arg(source.pr);
}

QString Title() {
    return QObject::tr("Tester build");
}

} // namespace

TesterBuildSettings::TesterBuildSettings(QWidget* parent) : QWidget(parent) {
    m_kind = new QComboBox;
    m_kind->addItem(Common::g_build_follow_branch[0] ? tr("This build") : tr("Release builds"));
    m_kind->addItem(tr("Tester build (branch or pull request)"));
    auto* kind_row = new QHBoxLayout;
    kind_row->addWidget(new QLabel(tr("Build source")));
    kind_row->addWidget(m_kind, 1);

    m_source = new QComboBox;
    m_source->setEditable(true);
    m_source->setInsertPolicy(QComboBox::NoInsert);
    m_source->lineEdit()->setPlaceholderText(tr("Branch name, #838 or a pull request link"));
    auto* source_row = new QHBoxLayout;
    source_row->addWidget(new QLabel(tr("Branch or pull request")));
    source_row->addWidget(m_source, 1);

    m_message = new QLabel;
    m_message->setWordWrap(true);
    m_status = new QLabel;
    m_status->setWordWrap(true);

    m_check = new QPushButton(tr("Check now"));
    m_build = new QPushButton(tr("Build now"));
    m_delete = new QPushButton;
    auto* button_row = new QHBoxLayout;
    button_row->addWidget(m_check);
    button_row->addWidget(m_build);
    button_row->addWidget(m_delete);
    button_row->addStretch();

    m_tester = new QWidget;
    auto* tester_layout = new QVBoxLayout(m_tester);
    tester_layout->setContentsMargins(0, 0, 0, 0);
    tester_layout->addLayout(source_row);
    tester_layout->addWidget(m_message);
    tester_layout->addWidget(m_status);
    tester_layout->addLayout(button_row);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(kind_row);
    layout->addWidget(m_tester);

    if (TesterLauncher::IsTesterBuild()) {
        m_delete->setEnabled(false);
        m_delete->setToolTip(tr("This is a tester build and cannot delete itself. Start the "
                                "release build to delete tester builds."));
    }
    m_delete->setText(tr("Delete tester builds"));

    connect(m_kind, &QComboBox::currentIndexChanged, this, &TesterBuildSettings::OnKindChanged);
    connect(m_source->lineEdit(), &QLineEdit::textChanged, this, [this] {
        m_check_text.clear();
        m_up_to_date = false;
        UpdateValidation();
        UpdateStatus();
    });
    connect(m_check, &QPushButton::clicked, this, &TesterBuildSettings::CheckNow);
    connect(m_build, &QPushButton::clicked, this, &TesterBuildSettings::BuildNow);
    connect(m_delete, &QPushButton::clicked, this, &TesterBuildSettings::DeleteBuilds);

    connect(&m_branches_watcher, &QFutureWatcher<QStringList>::finished, this, [this] {
        const auto names = m_branches_watcher.result();
        const auto text = m_source->currentText();
        m_source->clear();
        m_source->addItems(names);
        m_source->setEditText(text);
    });
    connect(&m_check_watcher, &QFutureWatcher<TesterLauncher::RemoteResult>::finished, this, [this] {
        const auto result = m_check_watcher.result();
        UpdateValidation();
        if (m_checked_source != EnteredSource()) {
            m_check_text.clear();
            UpdateStatus();
            return;
        }
        const auto state = TesterBuild::LoadState(TesterLauncher::StateFile());
        if (!result.info) {
            m_check_text = result.status != 0 ? tr("check failed (HTTP error %1)").arg(result.status)
                                              : tr("check failed (could not reach GitHub)");
        } else if (!result.info->pr_open || result.info->pr_merged) {
            m_check_text = result.info->pr_merged ? tr("pull request was merged")
                                                  : tr("pull request is closed");
        } else if (result.info->sha != state.built_sha || !TesterBuild::HasInstall(state) ||
                   SavedSource() != EnteredSource()) {
            m_check_text = tr("new commit available (%1)")
                               .arg(QString::fromStdString(TesterBuild::ShortSha(result.info->sha)));
        } else {
            m_check_text = tr("up to date");
            m_up_to_date = true;
        }
        UpdateStatus();
    });
    connect(&m_size_watcher, &QFutureWatcher<quint64>::finished, this, [this] {
        m_delete->setText(
            tr("Delete tester builds (%1)").arg(QLocale().formattedDataSize(m_size_watcher.result())));
    });

    ShowSource(SavedSource());
    if (const auto root = TesterBuild::DefaultRoot(); !root.empty()) {
        m_size_watcher.setFuture(QtConcurrent::run([root] { return quint64{TesterBuild::DiskUsage(root)}; }));
    }
}

TesterBuildSettings::~TesterBuildSettings() = default;

bool TesterBuildSettings::IsTesterChosen() const {
    return m_kind->currentIndex() == TesterIndex;
}

std::optional<TesterBuild::Source> TesterBuildSettings::EnteredSource() const {
    return IsTesterChosen() ? ParseEntered(m_source->currentText()) : std::nullopt;
}

std::optional<TesterBuild::Source> TesterBuildSettings::SavedSource() const {
    return TesterBuild::LoadState(TesterLauncher::StateFile()).source;
}

void TesterBuildSettings::ShowSource(const std::optional<TesterBuild::Source>& source) {
    m_kind->setCurrentIndex(source ? TesterIndex : ReleaseIndex);
    m_source->setEditText(source ? SourceText(*source) : QString{});
    OnKindChanged();
}

void TesterBuildSettings::OnKindChanged() {
    m_tester->setVisible(IsTesterChosen());
    if (IsTesterChosen()) {
        FillBranches();
    }
    UpdateValidation();
    UpdateStatus();
}

void TesterBuildSettings::FillBranches() {
    if (std::exchange(m_branches_requested, true)) {
        return;
    }
    m_branches_watcher.setFuture(QtConcurrent::run([] {
        QStringList names;
        for (const auto& name : Common::Net::GetBranches().names) {
            names << QString::fromStdString(name);
        }
        return names;
    }));
}

void TesterBuildSettings::UpdateValidation() {
    const auto text = m_source->currentText().trimmed();
    const bool invalid = IsTesterChosen() && !text.isEmpty() && !ParseEntered(text);
    m_message->setText(invalid ? tr("This is not a branch name, a pull request number such as "
                                    "#838, or a link to a pull request of this project.")
                               : QString{});
    m_message->setVisible(invalid);
    m_check->setEnabled(IsTesterChosen() && ParseEntered(text).has_value() &&
                        !m_check_watcher.isRunning());
}

void TesterBuildSettings::UpdateStatus() {
    const auto state = TesterBuild::LoadState(TesterLauncher::StateFile());
    const auto entered = EnteredSource();
    const bool built = entered && entered == state.source && TesterBuild::HasInstall(state);
    QString text = tr("Not built yet");
    if (built) {
        const auto when = QLocale().toString(QDateTime::fromSecsSinceEpoch(state.built_at),
                                             QLocale::ShortFormat);
        text = tr("Installed: %1 at %2, built %3")
                   .arg(QString::fromStdString(state.source->DisplayName()),
                        QString::fromStdString(TesterBuild::ShortSha(state.built_sha)), when);
    }
    if (!m_check_text.isEmpty()) {
        text += QStringLiteral(", ") + m_check_text;
    }
    m_status->setText(text);
    m_build->setText(m_up_to_date && built ? tr("Rebuild") : tr("Build now"));
    m_build->setEnabled(entered.has_value());
}

void TesterBuildSettings::CheckNow() {
    const auto source = EnteredSource();
    if (!source) {
        return;
    }
    m_check->setEnabled(false);
    m_checked_source = source;
    m_check_text = tr("checking...");
    UpdateStatus();
    m_check_watcher.setFuture(
        QtConcurrent::run([source = *source] { return TesterLauncher::FetchRemote(source); }));
}

bool TesterBuildSettings::WarnAboutPullRequest() {
    const auto source = EnteredSource();
    if (!source || source->kind != TesterBuild::Source::Kind::PullRequest || m_warning_accepted ||
        TesterBuild::LoadState(TesterLauncher::StateFile()).pr_warning_shown) {
        return true;
    }

    // The lookup runs in the background while the settings stay responsive.
    QFutureWatcher<TesterLauncher::RemoteResult> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcher<TesterLauncher::RemoteResult>::finished, &loop,
            &QEventLoop::quit);
    watcher.setFuture(
        QtConcurrent::run([source = *source] { return TesterLauncher::FetchRemote(source); }));
    QTimer::singleShot(LookupTimeoutMilliseconds, &loop, &QEventLoop::quit);
    loop.exec();

    QString which = QString::fromStdString(source->DisplayName());
    if (watcher.isFinished()) {
        const auto result = watcher.result();
        if (result.status == 404) {
            QMessageBox::warning(
                this, Title(),
                tr("%1 was not found in %2.")
                    .arg(which, QString::fromUtf8(Common::g_build_auto_update_repo)));
            return false;
        }
        if (result.info && !result.info->title.empty()) {
            which = tr("%1 \"%2\" by %3")
                        .arg(which, QString::fromStdString(result.info->title),
                             QString::fromStdString(result.info->author));
        }
    }
    const auto answer = QMessageBox::warning(
        this, Title(),
        tr("%1 will be built and run on this computer. This code has not been reviewed and may "
           "come from outside the project. Only continue if you trust it.")
            .arg(which),
        QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
    m_warning_accepted = answer == QMessageBox::Ok;
    return m_warning_accepted;
}

bool TesterBuildSettings::ConfirmChanges() {
    if (IsTesterChosen() && !EnteredSource()) {
        QMessageBox::warning(this, Title(),
                             tr("The tester build source is not valid. Enter a branch name, a "
                                "pull request number such as #838, or a link to a pull request of "
                                "this project, or choose Release builds."));
        m_source->setFocus();
        return false;
    }
    const auto entered = EnteredSource();
    const auto saved = SavedSource();
    if (entered && entered != saved && !WarnAboutPullRequest()) {
        ShowSource(saved);
    }
    return true;
}

bool TesterBuildSettings::SaveSource(const std::optional<TesterBuild::Source>& source) {
    auto state = TesterBuild::LoadState(TesterLauncher::StateFile());
    TesterBuild::SetSource(state, source);
    state.pr_warning_shown |= m_warning_accepted;
    std::error_code ec;
    if (!TesterBuild::SaveState(TesterLauncher::StateFile(), state, ec)) {
        QMessageBox::warning(this, Title(),
                             tr("Could not save the tester build settings: %1")
                                 .arg(QString::fromStdString(ec.message())));
        return false;
    }
    return true;
}

void TesterBuildSettings::Apply() {
    const auto entered = EnteredSource();
    if (IsTesterChosen() && !entered) {
        return;
    }
    const auto state = TesterBuild::LoadState(TesterLauncher::StateFile());
    if (entered == state.source) {
        return;
    }
    if (entered && entered->kind == TesterBuild::Source::Kind::PullRequest &&
        !m_warning_accepted && !state.pr_warning_shown) {
        return;
    }
    if (!SaveSource(entered)) {
        return;
    }

    const auto exe = TesterLauncher::BuilderExe(state);
    if (entered) {
        if (QMessageBox::question(this, Title(),
                                  tr("Nothing is built for %1 yet. Build it now?")
                                      .arg(QString::fromStdString(entered->DisplayName()))) ==
                QMessageBox::Yes &&
            !TesterLauncher::StartBuilder(exe, false)) {
            QMessageBox::warning(this, Title(), tr("Could not start %1.").arg(exe));
        }
        return;
    }
    if (!TesterLauncher::IsTesterBuild()) {
        return;
    }
    const auto release = QString::fromUtf8(state.release_exe);
    if (release.isEmpty() || !QFile::exists(release)) {
        QMessageBox::information(this, Title(),
                                 tr("The release build was not found. Start it yourself to leave "
                                    "the tester build."));
        return;
    }
    if (QMessageBox::question(this, Title(),
                              tr("Restart into the release build now? This tester build closes."),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    // The release build starts once this one has closed and saved its settings.
    QPointer<QWidget> main_window = window()->parentWidget();
    connect(qApp, &QCoreApplication::aboutToQuit, qApp,
            [release] { TesterLauncher::StartDetached(release, {}); }, Qt::DirectConnection);
    if (main_window) {
        QMetaObject::invokeMethod(main_window, &QWidget::close, Qt::QueuedConnection);
    }
}

void TesterBuildSettings::BuildNow() {
    const auto requested = EnteredSource();
    if (!ConfirmChanges()) {
        return;
    }
    const auto entered = EnteredSource();
    // A declined warning puts the saved source back; that is not what was asked to build.
    if (!entered || entered != requested) {
        return;
    }
    const auto state = TesterBuild::LoadState(TesterLauncher::StateFile());
    if (entered != state.source && !SaveSource(entered)) {
        return;
    }
    const bool force = m_up_to_date && entered == state.source && TesterBuild::HasInstall(state);
    const auto exe = TesterLauncher::BuilderExe(state);
    if (!TesterLauncher::StartBuilder(exe, force, QString::fromStdString(entered->ToString()))) {
        QMessageBox::warning(this, Title(), tr("Could not start %1.").arg(exe));
    }
    UpdateStatus();
}

void TesterBuildSettings::DeleteBuilds() {
    const auto root = TesterBuild::DefaultRoot();
    if (root.empty()) {
        return;
    }
    if (QMessageBox::question(this, Title(),
                              tr("Delete all tester builds and their downloaded tools? The "
                                 "release build is not affected."),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) !=
        QMessageBox::Yes) {
        return;
    }
    std::error_code ec;
    const bool removed = TesterBuild::RemoveAll(TesterBuild::MakeLayout(root), ec);

    auto state = TesterBuild::LoadState(TesterLauncher::StateFile());
    state.built_sha.clear();
    state.current_install.clear();
    state.previous_install.clear();
    state.built_at = 0;
    std::error_code save_ec;
    TesterBuild::SaveState(TesterLauncher::StateFile(), state, save_ec);

    if (!removed) {
        QMessageBox::warning(this, Title(),
                             tr("Some files could not be deleted: %1")
                                 .arg(QString::fromStdString(ec.message())));
    }
    m_check_text.clear();
    m_up_to_date = false;
    UpdateStatus();
    m_size_watcher.setFuture(
        QtConcurrent::run([root] { return quint64{TesterBuild::DiskUsage(root)}; }));
}
