// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Controller input through the virtual gamepad engine, which every emulated controller always
// listens to on its own port (no remapping needed), plus readback of what HID actually sees.

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

#include <fmt/format.h>

#include "core/core.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/touch_screen.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "mcp/context.h"
#include "mcp/host.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"

namespace Mcp {

namespace {

using VirtualButton = InputCommon::VirtualGamepad::VirtualButton;
using VirtualStick = InputCommon::VirtualGamepad::VirtualStick;

/// Button names accepted by the tools, mapped to virtual gamepad buttons.
constexpr std::array<std::pair<std::string_view, VirtualButton>, 20> kButtons{{
    {"A", VirtualButton::ButtonA},
    {"B", VirtualButton::ButtonB},
    {"X", VirtualButton::ButtonX},
    {"Y", VirtualButton::ButtonY},
    {"LSTICK", VirtualButton::StickL},
    {"RSTICK", VirtualButton::StickR},
    {"L", VirtualButton::TriggerL},
    {"R", VirtualButton::TriggerR},
    {"ZL", VirtualButton::TriggerZL},
    {"ZR", VirtualButton::TriggerZR},
    {"PLUS", VirtualButton::ButtonPlus},
    {"MINUS", VirtualButton::ButtonMinus},
    {"DLEFT", VirtualButton::ButtonLeft},
    {"DUP", VirtualButton::ButtonUp},
    {"DRIGHT", VirtualButton::ButtonRight},
    {"DDOWN", VirtualButton::ButtonDown},
    {"SL", VirtualButton::ButtonSL},
    {"SR", VirtualButton::ButtonSR},
    {"HOME", VirtualButton::ButtonHome},
    {"CAPTURE", VirtualButton::ButtonCapture},
}};

/// JSON array of every button name in kButtons, for schema enums.
json ButtonEnum() {
    json names = json::array();
    for (const auto& [name, button] : kButtons) {
        names.push_back(name);
    }
    return names;
}

/// Looks up a button by its exact (upper case) name. Callers dereference the result directly,
/// relying on the schema enum to have rejected unknown names.
std::optional<VirtualButton> ParseButton(std::string_view name) {
    for (const auto& [n, button] : kButtons) {
        if (n == name) {
            return button;
        }
    }
    return std::nullopt;
}

/// Returns the frontend's virtual gamepad, or nullptr with error set if there is no host or
/// input subsystem.
InputCommon::VirtualGamepad* Gamepad(ToolContext& ctx, std::string& error) {
    if (!ctx.host) {
        error = "no frontend host";
        return nullptr;
    }
    auto* input = ctx.host->GetInputSubsystem();
    if (!input) {
        error = "this frontend has no input subsystem";
        return nullptr;
    }
    return input->GetVirtualGamepad();
}

/// Sleeps for ms clamped to 0..60000, returning early if the server is stopping.
void SleepMs(int ms) {
    SleepUnlessStopping(std::chrono::milliseconds{std::clamp(ms, 0, 60000)});
}

/// Records an Input event on the timeline, if one is attached.
void LogInput(ToolContext& ctx, const std::string& what) {
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::Input, 0, "mcp", what);
    }
}

json StickArray() {
    return json{{"type", "array"},
                {"items", {{"type", "number"}, {"minimum", -1}, {"maximum", 1}}},
                {"minItems", 2},
                {"maxItems", 2},
                {"description", "[x, y], each in -1..1 (up is +y)."}};
}

/// Joins a JSON array of button names with "+" (e.g. "ZL+ZR") for log and reply text.
std::string JoinButtons(const json& buttons) {
    std::string out;
    for (const auto& b : buttons) {
        out += (out.empty() ? "" : "+") + b.get<std::string>();
    }
    return out;
}

/// press_buttons: holds all buttons together for hold_ms, releases them, and repeats with gap_ms
/// between presses. Blocks the calling worker thread for the whole duration.
ToolResult PressButtons(ToolContext& ctx, const json& args) {
    std::string error;
    auto* pad = Gamepad(ctx, error);
    if (!pad) {
        return ToolResult::Error(error);
    }
    const size_t npad = NpadIndex(args);
    std::vector<VirtualButton> buttons;
    for (const auto& b : args.at("buttons")) {
        buttons.push_back(*ParseButton(b.get<std::string>()));
    }
    const int hold_ms = args.value("hold_ms", 120);
    const int repeat = args.value("repeat", 1);
    const int gap_ms = args.value("gap_ms", 120);
    for (int i = 0; i < repeat; ++i) {
        for (const auto b : buttons) {
            pad->SetButtonState(npad, b, true);
        }
        SleepMs(hold_ms);
        for (const auto b : buttons) {
            pad->SetButtonState(npad, b, false);
        }
        if (i + 1 < repeat) {
            SleepMs(gap_ms);
        }
    }
    LogInput(ctx, fmt::format("press {} x{} hold {}ms (player npad {})",
                              JoinButtons(args.at("buttons")), repeat, hold_ms, npad));
    return ToolResult::Text(fmt::format("pressed {} x{}", JoinButtons(args.at("buttons")), repeat));
}

/// set_stick: moves a stick and either recenters it after hold_ms or, with hold_ms 0, leaves it
/// latched until the next call.
ToolResult SetStick(ToolContext& ctx, const json& args) {
    std::string error;
    auto* pad = Gamepad(ctx, error);
    if (!pad) {
        return ToolResult::Error(error);
    }
    const size_t npad = NpadIndex(args);
    const auto stick = args.at("stick").get<std::string>() == "right" ? VirtualStick::Right
                                                                      : VirtualStick::Left;
    const float x = args.at("x").get<float>();
    const float y = args.at("y").get<float>();
    const int hold_ms = args.value("hold_ms", 0);
    pad->SetStickPosition(npad, stick, x, y);
    if (hold_ms > 0) {
        SleepMs(hold_ms);
        pad->SetStickPosition(npad, stick, 0.0f, 0.0f);
    }
    LogInput(ctx, fmt::format("stick {} ({:.2f},{:.2f}) {}", args.at("stick").get<std::string>(),
                              x, y, hold_ms > 0 ? fmt::format("for {}ms", hold_ms) : "latched"));
    return ToolResult::Text(hold_ms > 0 ? "stick moved and released"
                                        : "stick latched (call again with 0,0 to center)");
}

/// Total wall-clock length of a step list in ms, using the same defaults as RunInputSteps.
long long StepsDurationMs(const json& steps) {
    long long total_ms = 0;
    for (const auto& step : steps) {
        total_ms += step.value("hold_ms", 120) + step.value("wait_ms", 0);
    }
    return total_ms;
}

/// input_sequence: runs the steps on one player and reports their total duration.
ToolResult InputSequence(ToolContext& ctx, const json& args) {
    const auto& steps = args.at("steps");
    if (std::string error = RunInputSteps(ctx, NpadIndex(args), steps); !error.empty()) {
        return ToolResult::Error(error);
    }
    return ToolResult::Text(
        fmt::format("ran {} steps in ~{} ms", steps.size(), StepsDurationMs(steps)));
}

/// release_all: releases every virtual button and centers all sticks on every player.
ToolResult ReleaseAll(ToolContext& ctx, const json&) {
    std::string error;
    auto* pad = Gamepad(ctx, error);
    if (!pad) {
        return ToolResult::Error(error);
    }
    pad->ResetControllers();
    LogInput(ctx, "release all");
    return ToolResult::Text("all virtual buttons released and sticks centered");
}

/// Returns the frontend's touch screen driver, or nullptr with error set if there is none.
InputCommon::TouchScreen* Touch(ToolContext& ctx, std::string& error) {
    auto* input = ctx.host ? ctx.host->GetInputSubsystem() : nullptr;
    if (!input) {
        error = "this frontend has no input subsystem";
        return nullptr;
    }
    return input->GetTouchScreen();
}

/// tap: presses finger 0 at (x, y) for hold_ms, `repeat` times with gap_ms between taps.
ToolResult Tap(ToolContext& ctx, const json& args) {
    std::string error;
    auto* touch = Touch(ctx, error);
    if (!touch) {
        return ToolResult::Error(error);
    }
    const float x = args.at("x").get<float>();
    const float y = args.at("y").get<float>();
    const int repeat = args.value("repeat", 1);
    for (int i = 0; i < repeat; ++i) {
        touch->TouchPressed(x, y, 0);
        SleepMs(args.value("hold_ms", 100));
        touch->TouchReleased(0);
        if (i + 1 < repeat) {
            SleepMs(args.value("gap_ms", 150));
        }
    }
    LogInput(ctx, fmt::format("tap ({:.3f},{:.3f}) x{}", x, y, repeat));
    return ToolResult::Text(fmt::format("tapped ({:.3f}, {:.3f})", x, y));
}

/// swipe: drags finger 0 in a straight line, moving it about every 16 ms over duration_ms, then
/// holds at the end for hold_end_ms before lifting.
ToolResult Swipe(ToolContext& ctx, const json& args) {
    std::string error;
    auto* touch = Touch(ctx, error);
    if (!touch) {
        return ToolResult::Error(error);
    }
    const auto& from = args.at("from");
    const auto& to = args.at("to");
    const int duration = args.value("duration_ms", 300);
    const int steps = std::max(2, duration / 16);
    const float x0 = from[0].get<float>(), y0 = from[1].get<float>();
    const float x1 = to[0].get<float>(), y1 = to[1].get<float>();
    touch->TouchPressed(x0, y0, 0);
    for (int i = 1; i <= steps; ++i) {
        SleepMs(duration / steps);
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        touch->TouchMoved(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, 0);
    }
    SleepMs(args.value("hold_end_ms", 50));
    touch->TouchReleased(0);
    LogInput(ctx, fmt::format("swipe ({:.2f},{:.2f}) -> ({:.2f},{:.2f}) {}ms", x0, y0, x1, y1,
                              duration));
    return ToolResult::Text("swiped");
}

/// get_controller_state: ControllerStateJson for the requested player, or an error if the npad
/// index has no emulated controller.
ToolResult ControllerState(ToolContext& ctx, const json& args) {
    json state = ControllerStateJson(ctx, NpadIndex(args));
    if (state.is_null()) {
        return ToolResult::Error("no such controller");
    }
    state["note"] = "Values as the game sees them (sticks in -32767..32767).";
    return ToolResult::Structured(std::move(state));
}

} // namespace

/// Schema of the shared `player` argument: 1-8, or 9 for handheld.
json PlayerSchema() {
    return json{{"description", "1-8 for players 1-8 (default 1), or 9 for handheld mode."},
                {"type", "integer"},
                {"minimum", 1},
                {"maximum", 9}};
}

size_t NpadIndex(const json& args) {
    const int player = args.value("player", 1);
    return static_cast<size_t>(player - 1); // 9 -> 8 = handheld
}

json InputStepsSchema() {
    return json{{"type", "array"},
                {"minItems", 1},
                {"maxItems", 200},
                {"items",
                 {{"type", "object"},
                  {"properties",
                   {{"buttons", {{"type", "array"},
                                 {"items", {{"type", "string"}, {"enum", ButtonEnum()}}}}},
                    {"left", StickArray()},
                    {"right", StickArray()},
                    {"hold_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 30000}}},
                    {"wait_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 30000}}}}},
                  {"additionalProperties", false}}}};
}

/// Presses, holds and releases each step's buttons and sticks in turn (at most 60 s in total).
/// Returns an error message, or empty on success.
std::string RunInputSteps(ToolContext& ctx, size_t npad, const json& steps) {
    std::string error;
    auto* pad = Gamepad(ctx, error);
    if (!pad) {
        return error;
    }
    const long long total_ms = StepsDurationMs(steps);
    if (total_ms > 60000) {
        return "sequence longer than 60 s; split it into several calls";
    }
    // Background work passes a stop token: a stopped route lets go of the pad within ~50 ms.
    const auto wait = [&](int ms) {
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds{std::clamp(ms, 0, 60000)};
        while (std::chrono::steady_clock::now() < until) {
            if (ServerStopping() || ctx.stop.stop_requested()) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return true;
    };
    for (const auto& step : steps) {
        std::vector<VirtualButton> buttons;
        for (const auto& b : step.value("buttons", json::array())) {
            buttons.push_back(*ParseButton(b.get<std::string>()));
        }
        for (const auto b : buttons) {
            pad->SetButtonState(npad, b, true);
        }
        if (step.contains("left")) {
            pad->SetStickPosition(npad, VirtualStick::Left, step["left"][0].get<float>(),
                                  step["left"][1].get<float>());
        }
        if (step.contains("right")) {
            pad->SetStickPosition(npad, VirtualStick::Right, step["right"][0].get<float>(),
                                  step["right"][1].get<float>());
        }
        const bool held = wait(step.value("hold_ms", 120));
        for (const auto b : buttons) {
            pad->SetButtonState(npad, b, false);
        }
        if (step.contains("left")) {
            pad->SetStickPosition(npad, VirtualStick::Left, 0.0f, 0.0f);
        }
        if (step.contains("right")) {
            pad->SetStickPosition(npad, VirtualStick::Right, 0.0f, 0.0f);
        }
        if (!held || !wait(step.value("wait_ms", 0))) {
            return "stopped";
        }
    }
    LogInput(ctx, fmt::format("sequence of {} steps ({} ms)", steps.size(), total_ms));
    return {};
}

/// Connection, style, pressed buttons and stick positions of one controller, or null.
json ControllerStateJson(ToolContext& ctx, size_t npad) {
    if (!ctx.system) {
        return nullptr;
    }
    auto* controller = ctx.system->HIDCore().GetEmulatedControllerByIndex(npad);
    if (!controller) {
        return nullptr;
    }
    const auto state = controller->GetNpadButtons();
    json pressed = json::array();
    const u64 raw = static_cast<u64>(state.raw);
    const auto bit = [raw](u32 index) { return ((raw >> index) & 1) != 0; };
    static constexpr std::array<std::string_view, 16> names{
        "A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R",
        "ZL", "ZR", "PLUS", "MINUS", "DLEFT", "DUP", "DRIGHT", "DDOWN"};
    for (u32 i = 0; i < names.size(); ++i) {
        if (bit(i)) {
            pressed.push_back(names[i]);
        }
    }
    const auto sticks = controller->GetSticks();
    return json{
        {"player", npad == 8 ? "handheld" : std::to_string(npad + 1)},
        {"connected", controller->IsConnected()},
        {"style", static_cast<int>(controller->GetNpadStyleIndex())},
        {"buttons", pressed},
        {"left_stick", {sticks.left.x, sticks.left.y}},
        {"right_stick", {sticks.right.x, sticks.right.y}},
    };
}

/// Registers the button, stick, touch, input sequence and controller state tools.
void RegisterInputTools(ToolRegistry& registry) {
    const json button_list = {{"type", "array"},
                              {"items", {{"type", "string"}, {"enum", ButtonEnum()}}},
                              {"minItems", 1},
                              {"maxItems", 8}};

    registry.Add(Tool{
        .name = "press_buttons",
        .title = "Press buttons",
        .description = "Press and release controller buttons together (e.g. [\"A\"] or "
                       "[\"ZL\",\"ZR\"]). hold_ms is wall-clock: games read input once per frame, "
                       "so keep it >= 50 ms (default 120). Physical controllers keep working; the "
                       "last pressed source wins per button.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"buttons", button_list},
                           {"hold_ms", {{"type", "integer"}, {"minimum", 16}, {"maximum", 10000}}},
                           {"repeat", {{"type", "integer"}, {"minimum", 1}, {"maximum", 50}}},
                           {"gap_ms", {{"type", "integer"}, {"minimum", 16}, {"maximum", 10000}}},
                           {"player", PlayerSchema()}}},
                         {"required", {"buttons"}},
                         {"additionalProperties", false}},
        .annotations = {},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = PressButtons,
    });

    registry.Add(Tool{
        .name = "set_stick",
        .title = "Move a stick",
        .description = "Set the left or right stick to (x, y) in -1..1 (up is +y). With hold_ms "
                       "it returns to center afterwards; without, it stays there until moved again.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"stick", {{"type", "string"}, {"enum", {"left", "right"}}}},
                           {"x", {{"type", "number"}, {"minimum", -1}, {"maximum", 1}}},
                           {"y", {{"type", "number"}, {"minimum", -1}, {"maximum", 1}}},
                           {"hold_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 30000}}},
                           {"player", PlayerSchema()}}},
                         {"required", {"stick", "x", "y"}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = SetStick,
    });

    registry.Add(Tool{
        .name = "input_sequence",
        .title = "Run an input sequence",
        .description = "Run timed steps in order, each pressing buttons and/or holding sticks for "
                       "hold_ms then waiting wait_ms (max 200 steps, 60 s total). For "
                       "frame-exact input, use TAS tools instead (coming).",
        .input_schema = {{"type", "object"},
                         {"properties", {{"steps", InputStepsSchema()}, {"player", PlayerSchema()}}},
                         {"required", {"steps"}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = InputSequence,
    });

    registry.Add(Tool{
        .name = "release_all",
        .title = "Release all inputs",
        .description = "Release every virtual button and center both sticks on all players.",
        .annotations = {.idempotent = true},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = ReleaseAll,
    });

    const json coord = {{"type", "number"}, {"minimum", 0}, {"maximum", 1}};
    const json point = {{"type", "array"}, {"items", coord}, {"minItems", 2}, {"maxItems", 2},
                        {"description", "[x, y] in 0..1 of the screen (0,0 = top left)."}};
    registry.Add(Tool{
        .name = "tap",
        .title = "Tap the touch screen",
        .description = "Touch the console screen at (x, y) in 0..1 (top left = 0,0) for hold_ms "
                       "(handheld touch screen; many menus accept it).",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"x", coord},
                           {"y", coord},
                           {"hold_ms", {{"type", "integer"}, {"minimum", 16}, {"maximum", 10000}}},
                           {"repeat", {{"type", "integer"}, {"minimum", 1}, {"maximum", 20}}},
                           {"gap_ms", {{"type", "integer"}, {"minimum", 16}, {"maximum", 5000}}}}},
                         {"required", {"x", "y"}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = Tap,
    });
    registry.Add(Tool{
        .name = "swipe",
        .title = "Swipe the touch screen",
        .description = "Drag one finger from 'from' to 'to' (0..1 coordinates) over duration_ms.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"from", point},
                           {"to", point},
                           {"duration_ms", {{"type", "integer"}, {"minimum", 32}, {"maximum", 10000}}},
                           {"hold_end_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 5000}}}}},
                         {"required", {"from", "to"}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = Swipe,
    });

    registry.Add(Tool{
        .name = "get_controller_state",
        .title = "Controller state",
        .description = "Buttons pressed and stick positions as the game currently sees them, plus "
                       "whether the controller is connected. Use it to verify input landed.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"player", PlayerSchema()}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .requires_game = true,
        .handler = ControllerState,
    });
}

} // namespace Mcp
