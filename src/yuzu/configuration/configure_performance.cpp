// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <fstream>
#include <functional>
#include <QCheckBox>
#include <QFontDatabase>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "yuzu/configuration/shared_widget.h"
#include "yuzu/configuration/configure_performance.h"

namespace Performance {

namespace {

constexpr const char* BlockBegin =
    "# >>> Performance tab (managed by Eglantine; changes inside this block are overwritten) >>>";
constexpr const char* BlockEnd = "# <<< Performance tab <<<";

constexpr u32 VendorNvidia = 0x10DE;
constexpr u32 VendorAmd = 0x1002;
constexpr u32 VendorIntel = 0x8086;

constexpr const char* ExternalMemoryHost = "VK_EXT_external_memory_host";

struct Context {
    bool windows{};
    bool linux_os{};
    bool macos{};
    const VkDeviceInfo::Record* device{}; ///< The primary graphics device, null if unknown
};

/// Empty strings mean nothing to report.
struct Verdict {
    QString unsupported; ///< Why the argument cannot work in this configuration
    QString note;        ///< Something worth knowing; it still works
};

struct Feature {
    const char* name;
    const char* value;
    const char* title;
    const char* description;
    bool recommended;
    std::function<Verdict(const Context&)> check;
};

std::string Line(const Feature& feature) {
    return std::string{feature.name} + "=" + feature.value;
}

Verdict HostImportVerdict(const Context& context) {
    Verdict verdict;
    if (context.device != nullptr) {
        const auto& caps = context.device->capabilities;
        if (!caps.HasExtension(ExternalMemoryHost)) {
            verdict.unsupported =
                QObject::tr("The driver of %1 does not offer VK_EXT_external_memory_host.")
                    .arg(QString::fromStdString(context.device->name));
            return verdict;
        }
        if (caps.driver_id == VK_DRIVER_ID_MESA_RADV && context.linux_os) {
            verdict.unsupported = QObject::tr(
                "RADV cannot import the memory the emulator keeps the guest's RAM in "
                "(measured: VK_ERROR_INVALID_EXTERNAL_HANDLE), so it would copy as before.");
            return verdict;
        }
    } else {
        verdict.note = QObject::tr("Support could not be checked: no graphics device was detected.");
    }
    if (context.windows) {
        verdict.note = QObject::tr("Not verified on Windows yet. If the driver refuses the "
                                   "import, the emulator copies as before.");
    } else if (context.macos) {
        verdict.note = QObject::tr("Not verified on macOS. If the driver refuses the import, the "
                                   "emulator copies as before.");
    }
    return verdict;
}

std::vector<Feature> FeaturesFor(Platform platform) {
    std::vector<Feature> features;
    const auto none = [](const Context&) { return Verdict{}; };
    features.push_back(
        {"EDEN_NX2_SPLIT", "2", QT_TR_NOOP("Run the GPU commands on a second thread"),
         QT_TR_NOOP("The GPU thread parses command lists and a back thread runs the engine "
                    "methods, macros and draws in order. Up to +54% FPS in Switch 2 games. "
                    "Already on by default; listed so it stays on."),
         true, none});
    features.push_back(
        {"EDEN_NX2_FENCE_BATCH", "1", QT_TR_NOOP("Release GPU fences in batches"),
         QT_TR_NOOP("Switch 2 games signal thousands of fences a second; releasing the ones the "
                    "host GPU already passed together smooths frame times."),
         true, none});
    const bool host_import_recommended = platform == Platform::Nvidia;
    features.push_back(
        {"EDEN_NX2_HOST_IMPORT", "1", QT_TR_NOOP("Read uniform banks straight from guest memory"),
         QT_TR_NOOP("Native 64 KiB uniform banks are bound from the guest's own memory with no "
                    "copy and no re-upload (about 2% faster in Ocarina of Time on NVIDIA, no "
                    "visual change). Uses VK_EXT_external_memory_host; the emulator copies "
                    "instead when the driver refuses. On by default with the NVIDIA proprietary "
                    "driver on Linux; add EDEN_NX2_HOST_IMPORT=0 to turn it off there."),
         host_import_recommended, HostImportVerdict});
    if (platform == Platform::Nvidia) {
        features.push_back(
            {"EDEN_NX2_BANK_REUPLOAD", "0", QT_TR_NOOP("Skip the once-per-frame bank re-upload"),
             QT_TR_NOOP("Saves about 5% in the Kokiri Forest vista. The re-upload exists to "
                        "prevent one-frame white flashes, so they may come back. Not part of the "
                        "recommended defaults."),
             false, [](const Context&) {
                 Verdict verdict;
                 verdict.note = QObject::tr("May bring back occasional white flashes.");
                 return verdict;
             }});
    }
    return features;
}

QString PlatformName(Platform platform) {
    switch (platform) {
    case Platform::Nvidia:
        return QStringLiteral("NVIDIA");
    case Platform::Amd:
        return QStringLiteral("AMD");
    case Platform::Intel:
        return QStringLiteral("Intel");
    case Platform::MacOs:
        return QStringLiteral("MacOS");
    }
    return {};
}

u32 PlatformVendor(Platform platform) {
    switch (platform) {
    case Platform::Nvidia:
        return VendorNvidia;
    case Platform::Amd:
        return VendorAmd;
    case Platform::Intel:
        return VendorIntel;
    case Platform::MacOs:
        return 0;
    }
    return 0;
}

Context MakeContext(const std::vector<VkDeviceInfo::Record>& records) {
    Context context;
#ifdef _WIN32
    context.windows = true;
#elif defined(__APPLE__)
    context.macos = true;
#else
    context.linux_os = true;
#endif
    const u32 index = Settings::values.vulkan_device.GetValue();
    if (index < records.size()) {
        context.device = &records[index];
    }
    return context;
}

QString WarningStyle() {
    return QStringLiteral("color: #d98e00;");
}

} // Anonymous namespace

State::State(QObject* parent) : QObject(parent) {
    std::ifstream file{SwitchFilePath()};
    std::string text;
    bool inside = false;
    while (std::getline(file, text)) {
        while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) {
            text.pop_back();
        }
        if (text == BlockBegin) {
            inside = true;
        } else if (text == BlockEnd) {
            inside = false;
        } else if (inside && !text.empty() && text[0] != '#') {
            enabled.insert(text);
        }
    }
}

std::filesystem::path State::SwitchFilePath() {
    // The user copy the emulator reads at start (main.cpp, LoadNx2Switches).
#ifdef _WIN32
    const auto config_dir = Common::FS::GetAppDataRoamingDirectory() / EDEN_DIR;
#else
    const auto config_dir = Common::FS::GetDataDirectory("XDG_CONFIG_HOME") / EDEN_DIR;
#endif
    return config_dir / "nx2_switches.txt";
}

bool State::IsEnabled(const std::string& line) const {
    return enabled.contains(line);
}

void State::SetEnabled(const std::string& line, bool on) {
    if (on ? enabled.insert(line).second : enabled.erase(line) != 0) {
        emit Changed();
    }
}

void State::Apply() const {
    const auto path = SwitchFilePath();
    std::vector<std::string> kept;
    {
        std::ifstream file{path};
        std::string text;
        bool inside = false;
        while (std::getline(file, text)) {
            std::string trimmed = text;
            while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == ' ')) {
                trimmed.pop_back();
            }
            if (trimmed == BlockBegin) {
                inside = true;
            } else if (trimmed == BlockEnd) {
                inside = false;
            } else if (!inside) {
                kept.push_back(text);
            }
        }
    }
    while (!kept.empty() && kept.back().empty()) {
        kept.pop_back();
    }
    if (kept.empty() && enabled.empty()) {
        return; // Nothing to write, and no file to create
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out{path, std::ios::trunc};
    if (!out) {
        LOG_ERROR(Frontend, "Could not write {}", path.string());
        return;
    }
    for (const auto& text : kept) {
        out << text << '\n';
    }
    if (!enabled.empty()) {
        if (!kept.empty()) {
            out << '\n';
        }
        out << BlockBegin << '\n';
        for (const auto& line : enabled) {
            out << line << '\n';
        }
        out << BlockEnd << '\n';
    }
}

} // namespace Performance

ConfigurePerformance::ConfigurePerformance(Performance::Platform platform_,
                                           Performance::State& state_,
                                           const std::vector<VkDeviceInfo::Record>& records,
                                           QWidget* parent)
    : QWidget(parent), platform{platform_}, state{state_} {
    using namespace Performance;
    setAccessibleName(PlatformName(platform));
    const Context context = MakeContext(records);

    auto* const outer = new QVBoxLayout(this);
    auto* const scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* const content = new QWidget(scroll);
    scroll->setWidget(content);
    auto* const layout = new QVBoxLayout(content);

    auto* const title = new QLabel(tr("Recommended launch arguments for %1").arg(PlatformName(platform)));
    QFont title_font = title->font();
    title_font.setBold(true);
    title->setFont(title_font);
    layout->addWidget(title);

    // The primary graphics device against this platform.
    const QString device_name =
        context.device != nullptr ? QString::fromStdString(context.device->name) : QString{};
    auto* const device_label = new QLabel(
        context.device != nullptr ? tr("Primary graphics device: %1").arg(device_name)
                                  : tr("Primary graphics device: not detected"));
    device_label->setWordWrap(true);
    layout->addWidget(device_label);

    QString device_warning;
    if (platform == Platform::MacOs) {
        if (!context.macos) {
            device_warning = tr("This is not macOS. These arguments are meant for macOS "
                                "(MoltenVK) and may have no effect or slow the game down here.");
        }
    } else if (context.device == nullptr) {
        device_warning = tr("No primary graphics device was detected, so it is unknown whether "
                            "these arguments fit it. They may have no effect or slow the game "
                            "down.");
    } else if (context.device->capabilities.vendor_id != PlatformVendor(platform)) {
        device_warning =
            tr("Your primary graphics device (%1) is not a %2 GPU. These arguments were chosen "
               "for %2 hardware and may have no effect or a negative effect on your gameplay.")
                .arg(device_name, PlatformName(platform));
    }
    if (!device_warning.isEmpty()) {
        auto* const warning = new QLabel(QStringLiteral("⚠ ") + device_warning);
        warning->setWordWrap(true);
        warning->setStyleSheet(QStringLiteral("QLabel { %1 border: 1px solid #d98e00; "
                                              "border-radius: 4px; padding: 6px; }")
                                   .arg(WarningStyle()));
        layout->addWidget(warning);
    }

    auto* const group = new QGroupBox(tr("Launch arguments"));
    auto* const group_layout = new QVBoxLayout(group);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    for (const Feature& feature : FeaturesFor(platform)) {
        const Verdict verdict = feature.check(context);
        Row row;
        row.line = Line(feature);
        row.box = new QCheckBox(tr(feature.title));
        if (!verdict.unsupported.isEmpty()) {
            row.box->setText(row.box->text() + QStringLiteral("  ⚠"));
        }
        group_layout->addWidget(row.box);

        auto* const argument = new QLabel(QString::fromStdString(row.line));
        argument->setFont(mono);
        argument->setContentsMargins(24, 0, 0, 0);
        argument->setTextInteractionFlags(Qt::TextSelectableByMouse);
        group_layout->addWidget(argument);

        auto* const description = new QLabel(tr(feature.description));
        description->setWordWrap(true);
        description->setContentsMargins(24, 0, 0, 0);
        group_layout->addWidget(description);

        const auto add_message = [&](const QString& text, const QString& icon) {
            auto* const label = new QLabel(icon + text);
            label->setWordWrap(true);
            label->setContentsMargins(24, 0, 0, 0);
            label->setStyleSheet(QStringLiteral("QLabel { %1 }").arg(WarningStyle()));
            group_layout->addWidget(label);
        };
        if (!verdict.unsupported.isEmpty()) {
            add_message(tr("Not supported with this configuration: %1").arg(verdict.unsupported),
                        QStringLiteral("⚠ "));
        } else if (!verdict.note.isEmpty()) {
            add_message(verdict.note, QStringLiteral("ⓘ "));
        }
        if (std::getenv(feature.name) != nullptr) {
            add_message(tr("%1 is already set in the environment, which overrides this file.")
                            .arg(QString::fromLatin1(feature.name)),
                        QStringLiteral("ⓘ "));
        }
        if (feature.recommended && verdict.unsupported.isEmpty()) {
            recommended_lines.push_back(row.line);
        }
        connect(row.box, &QCheckBox::toggled, this,
                [this, line = row.line](bool on) { state.SetEnabled(line, on); });
        rows.push_back(std::move(row));
        group_layout->addSpacing(6);
    }
    layout->addWidget(group);

    auto* const defaults = new QPushButton(tr("Add platform recommended defaults"));
    connect(defaults, &QPushButton::clicked, this, &ConfigurePerformance::AddDefaults);
    layout->addWidget(defaults, 0, Qt::AlignLeft);

    auto* const footer = new QLabel(
        tr("Saved to %1 when you press OK or Apply, and read when the emulator starts: restart "
           "it for changes to take effect. The recommendations come from measurements on "
           "NVIDIA hardware on Linux; other platforms have not been measured.")
            .arg(QString::fromStdString(State::SwitchFilePath().string())));
    footer->setWordWrap(true);
    layout->addWidget(footer);
    layout->addStretch();

    connect(&state, &State::Changed, this, &ConfigurePerformance::Refresh);
    Refresh();
}

ConfigurePerformance::~ConfigurePerformance() = default;

void ConfigurePerformance::AddDefaults() {
    for (const auto& line : recommended_lines) {
        state.SetEnabled(line, true);
    }
    Refresh();
}

void ConfigurePerformance::Refresh() {
    for (Row& row : rows) {
        const QSignalBlocker blocker(row.box);
        row.box->setChecked(state.IsEnabled(row.line));
    }
}

ConfigureRam::ConfigureRam(const Core::System& system_, const ConfigurationShared::Builder& builder,
                           QWidget* parent)
    : Tab(nullptr, parent), system{system_} {
    setAccessibleName(tr("RAM"));
    auto* const layout = new QVBoxLayout(this);
    auto* const general = new QGroupBox(tr("General"));
    auto* const general_layout = new QVBoxLayout(general);
    for (auto* const setting :
         Settings::values.linkage.by_category[Settings::Category::RendererMemory]) {
        ConfigurationShared::Widget* const widget = builder.BuildWidget(setting, apply_funcs);
        if (widget == nullptr) {
            continue;
        }
        if (!widget->Valid()) {
            widget->deleteLater();
            continue;
        }
        general_layout->addWidget(widget);
    }
    layout->addWidget(general);
    layout->addStretch(1);
}

ConfigureRam::~ConfigureRam() = default;

void ConfigureRam::ApplyConfiguration() {
    const bool is_powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(is_powered_on);
    }
}

void ConfigureRam::SetConfiguration() {}
