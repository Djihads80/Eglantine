// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "yuzu/configuration/shared_widget.h"
#include "yuzu/configuration/configure_performance.h"
#include "yuzu/nx2_switches.h"

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
    const auto measured_on_nvidia = [](const Context& context) {
        Verdict verdict;
        if (context.device == nullptr || context.device->capabilities.vendor_id != VendorNvidia ||
            !context.linux_os) {
            verdict.note = QObject::tr("Only measured on NVIDIA on Linux so far.");
        }
        return verdict;
    };
    features.push_back(
        {"EDEN_NX2_LIST_SPECULATE", "1",
         QT_TR_NOOP("Parse command lists without waiting for their writers"),
         QT_TR_NOOP("The GPU thread reads a command list as soon as it is submitted instead of "
                    "waiting for the GPU work queued before it, and checks afterwards that the "
                    "work did not change it. Removed about 20 ms of waiting per frame in Donkey "
                    "Kong Bananza's busy areas. A list that did change is only reported, not "
                    "undone, so a game could misread commands. Not part of the recommended "
                    "defaults."),
         false, measured_on_nvidia});
    features.push_back(
        {"EDEN_NX2_ACQ_BACK_CHECK", "1",
         QT_TR_NOOP("Check GPU channel handovers without stalling"),
         QT_TR_NOOP("When one GPU channel waits for another (an equal or mask semaphore acquire), "
                    "the wait is checked in order on the second GPU thread instead of stopping "
                    "the first to catch up. Halved those stalls in Donkey Kong Bananza. Not part "
                    "of the recommended defaults."),
         false, measured_on_nvidia});
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
    return Nx2Switches::UserFilePath();
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

    auto* const layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    // Whether this platform's arguments fit the primary graphics device.
    const QString device_name =
        context.device != nullptr ? QString::fromStdString(context.device->name) : QString{};
    QString device_warning;
    QString device_warning_detail;
    if (platform == Platform::MacOs) {
        if (!context.macos) {
            device_warning = tr("These flags are meant for macOS.");
            device_warning_detail = tr("This is not macOS. These arguments are meant for macOS "
                                       "(MoltenVK) and may have no effect or slow the game down "
                                       "here.");
        }
    } else if (context.device == nullptr) {
        device_warning = tr("No graphics device was detected.");
        device_warning_detail = tr("No primary graphics device was detected, so it is unknown "
                                   "whether these arguments fit it. They may have no effect or "
                                   "slow the game down.");
    } else if (context.device->capabilities.vendor_id != PlatformVendor(platform)) {
        device_warning = tr("Your primary GPU (%1) is not a %2 GPU.")
                             .arg(device_name, PlatformName(platform));
        device_warning_detail =
            tr("These arguments were chosen for %1 hardware and may have no effect or a negative "
               "effect on your gameplay.")
                .arg(PlatformName(platform));
    }
    if (!device_warning.isEmpty()) {
        auto* const warning = new QLabel(QStringLiteral("⚠ ") + device_warning);
        warning->setWordWrap(true);
        warning->setToolTip(device_warning_detail);
        warning->setStyleSheet(QStringLiteral("QLabel { %1 }").arg(WarningStyle()));
        layout->addWidget(warning);
    }

    // Launch argument | Status, one row per argument; the flag and details are in the tooltips.
    auto* const frame = new QFrame(this);
    frame->setFrameShape(QFrame::StyledPanel);
    auto* const grid = new QGridLayout(frame);
    grid->setContentsMargins(12, 8, 12, 8);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(0, 1);

    const auto add_header = [&](int column, const QString& text) {
        auto* const header = new QLabel(text, frame);
        QFont font = header->font();
        font.setBold(true);
        header->setFont(font);
        grid->addWidget(header, 0, column);
    };
    add_header(0, tr("Launch argument"));
    add_header(1, tr("Status"));
    auto* const rule = new QFrame(frame);
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Sunken);
    grid->addWidget(rule, 1, 0, 1, 2);

    int grid_row = 2;
    for (const Feature& feature : FeaturesFor(platform)) {
        const Verdict verdict = feature.check(context);
        const bool unsupported = !verdict.unsupported.isEmpty();
        const bool from_environment = std::getenv(feature.name) != nullptr;
        Row row;
        row.line = Line(feature);

        QStringList details{tr(feature.description)};
        if (unsupported) {
            details.append(tr("⚠ Not supported with this configuration: %1")
                               .arg(verdict.unsupported));
        } else if (!verdict.note.isEmpty()) {
            details.append(QStringLiteral("ⓘ ") + verdict.note);
        }
        if (from_environment) {
            details.append(tr("ⓘ %1 is already set in the environment, which overrides "
                              "this file.")
                               .arg(QString::fromLatin1(feature.name)));
        }
        const QString tooltip =
            QStringLiteral("<p><b>%1</b><br><code>%2</code></p><p>%3</p>")
                .arg(tr(feature.title).toHtmlEscaped(),
                     QString::fromStdString(row.line).toHtmlEscaped(),
                     details.join(QStringLiteral("\n")).toHtmlEscaped().replace(
                         QStringLiteral("\n"), QStringLiteral("</p><p>")));

        row.box = new QCheckBox(tr(feature.title), frame);
        row.box->setToolTip(tooltip);

        QString status_text;
        bool warn = false;
        if (unsupported) {
            status_text = tr("⚠ Unsupported");
            warn = true;
        } else if (from_environment) {
            status_text = tr("ⓘ Set by environment");
            warn = true;
        } else if (feature.recommended) {
            status_text = tr("Recommended");
        } else {
            status_text = tr("Optional");
        }
        if (!unsupported && !from_environment && !verdict.note.isEmpty()) {
            status_text += QStringLiteral(" ⓘ");
        }
        auto* const status = new QLabel(status_text, frame);
        status->setToolTip(tooltip);
        if (warn) {
            status->setStyleSheet(QStringLiteral("QLabel { %1 }").arg(WarningStyle()));
        } else if (feature.recommended) {
            QFont font = status->font();
            font.setBold(true);
            status->setFont(font);
        } else {
            status->setEnabled(false);
        }

        grid->addWidget(row.box, grid_row, 0);
        grid->addWidget(status, grid_row, 1);
        ++grid_row;

        if (feature.recommended && !unsupported) {
            recommended_lines.push_back(row.line);
        }
        connect(row.box, &QCheckBox::toggled, this,
                [this, line = row.line](bool on) { state.SetEnabled(line, on); });
        rows.push_back(std::move(row));
    }
    layout->addWidget(frame);

    auto* const buttons = new QHBoxLayout;
    auto* const defaults = new QPushButton(tr("Select Recommended"));
    defaults->setToolTip(tr("Ticks every recommended flag that works with this configuration."));
    connect(defaults, &QPushButton::clicked, this, &ConfigurePerformance::AddDefaults);
    auto* const none = new QPushButton(tr("Select None"));
    none->setToolTip(tr("Unticks every flag on this page."));
    connect(none, &QPushButton::clicked, this, [this] {
        for (const Row& row : rows) {
            state.SetEnabled(row.line, false);
        }
    });
    buttons->addWidget(defaults);
    buttons->addWidget(none);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    auto* const footer = new QLabel(tr("Hover a flag for details. Saved with OK or Apply; "
                                       "restart the emulator for changes to take effect."));
    footer->setWordWrap(true);
    footer->setEnabled(false);
    footer->setToolTip(
        tr("Written to %1, which the emulator reads when it starts. The recommendations come from "
           "measurements on NVIDIA hardware on Linux; other platforms have not been measured.")
            .arg(QString::fromStdString(State::SwitchFilePath().string())));
    layout->addWidget(footer);
    layout->addStretch(1);

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

ConfigureGpu::ConfigureGpu(Performance::State& state,
                           const std::vector<VkDeviceInfo::Record>& records, QWidget* parent)
    : QWidget(parent) {
    using namespace Performance;
    setAccessibleName(tr("GPU"));
    const Context context = MakeContext(records);

    constexpr std::array platforms{Platform::Nvidia, Platform::Amd, Platform::Intel,
                                   Platform::MacOs};
    // Start on the platform this machine is: macOS, else the primary device's vendor.
    Platform detected = Platform::Nvidia;
    bool have_detected = false;
    if (context.macos) {
        detected = Platform::MacOs;
        have_detected = true;
    } else if (context.device != nullptr) {
        for (const Platform platform : platforms) {
            if (PlatformVendor(platform) == context.device->capabilities.vendor_id) {
                detected = platform;
                have_detected = true;
            }
        }
    }

    auto* const layout = new QVBoxLayout(this);

    auto* const selector_box = new QGroupBox(tr("GPU Vendor"), this);
    auto* const selector = new QHBoxLayout(selector_box);
    auto* const selector_label = new QLabel(tr("Show recommendations for:"));
    selector->addWidget(selector_label);
    selector->addStretch(1);
    vendor_combo = new QComboBox(this);
    vendor_combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    vendor_combo->setToolTip(tr("Pick another vendor to compare its flags. Ticked flags of every "
                                "vendor are saved together."));
    selector_label->setBuddy(vendor_combo);
    pages = new QStackedWidget;
    for (const Platform platform : platforms) {
        QString text = PlatformName(platform);
        if (have_detected && platform == detected) {
            text = context.device != nullptr
                       ? tr("%1: %2 (primary GPU)")
                             .arg(text, QString::fromStdString(context.device->name))
                       : tr("%1 (this system)").arg(text);
        }
        vendor_combo->addItem(text);
        pages->addWidget(new ConfigurePerformance(platform, state, records, pages));
    }
    selector->addWidget(vendor_combo);
    layout->addWidget(selector_box);

    auto* const scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(pages);
    layout->addWidget(scroll, 1);

    connect(vendor_combo, &QComboBox::currentIndexChanged, pages,
            &QStackedWidget::setCurrentIndex);
    const int start = static_cast<int>(
        std::find(platforms.begin(), platforms.end(), detected) - platforms.begin());
    vendor_combo->setCurrentIndex(start);
    pages->setCurrentIndex(start);
}

ConfigureGpu::~ConfigureGpu() = default;

ConfigureShaders::ConfigureShaders(const Core::System& system_,
                                   const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(nullptr, parent), system{system_} {
    setAccessibleName(tr("Shaders"));
    auto* const layout = new QVBoxLayout(this);

    auto& values = Settings::values;
    const auto add_group = [&](const QString& title,
                               std::initializer_list<Settings::BasicSetting*> settings) {
        auto* const group = new QGroupBox(title);
        auto* const group_layout = new QVBoxLayout(group);
        for (Settings::BasicSetting* const setting : settings) {
            ConfigurationShared::Widget* const widget =
                setting == &values.driver_shader_cache_size
                    ? builder.BuildWidget(setting, apply_funcs, nullptr,
                                          ConfigurationShared::RequestType::SpinBox, tr(" GiB"))
                    : builder.BuildWidget(setting, apply_funcs);
            if (widget == nullptr) {
                continue;
            }
            if (!widget->Valid()) {
                widget->deleteLater();
                continue;
            }
            group_layout->addWidget(widget);
        }
        if (group_layout->count() == 0) {
            delete group;
            return;
        }
        layout->addWidget(group);
    };

    add_group(tr("Compilation"), {&values.use_asynchronous_shaders, &values.use_mesh_shaders});
    add_group(tr("Caches"), {&values.use_disk_shader_cache, &values.use_vulkan_driver_pipeline_cache,
                             &values.driver_shader_cache_size});
    layout->addStretch(1);
}

ConfigureShaders::~ConfigureShaders() = default;

bool ConfigureShaders::IsShaderSetting(u32 id) {
    const auto& v = Settings::values;
    return id == v.use_asynchronous_shaders.Id() || id == v.use_mesh_shaders.Id() ||
           id == v.use_disk_shader_cache.Id() || id == v.use_vulkan_driver_pipeline_cache.Id() ||
           id == v.private_driver_shader_cache.Id() || id == v.driver_shader_cache_size.Id();
}

void ConfigureShaders::ApplyConfiguration() {
    const bool is_powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(is_powered_on);
    }
}

void ConfigureShaders::SetConfiguration() {}
