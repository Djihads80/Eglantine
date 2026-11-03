# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Built-in MCP (Model Context Protocol) server. See tools/mcp/README.md.

add_library(mcp STATIC
    activity.h
    context.cpp
    control.cpp
    control.h
    context.h
    dashboard.h
    host.h
    protocol.cpp
    protocol.h
    server.cpp
    server.h
    timeline.cpp
    timeline.h
    tool.cpp
    tool.h
    tool_registry.cpp
    tool_registry.h
    tools/abtest_tools.cpp
    tools/act_tools.cpp
    tools/artifact_tools.cpp
    tools/core_tools.cpp
    tools/debugger_tools.cpp
    tools/emulation_tools.cpp
    tools/game_state_tools.cpp
    tools/input_tools.cpp
    tools/memory_tools.cpp
    tools/perf_tools.cpp
    tools/process_tools.cpp
    tools/debug_tools.cpp
    tools/control_tools.cpp
    tools/settings_tools.cpp
    tools/tas_tools.cpp
    tools/timeline_tools.cpp
    tools/trace_tools.cpp
    tools/tool_helpers.h
)

target_link_libraries(mcp PUBLIC common core PRIVATE hid_core input_common httplib::httplib nlohmann_json::nlohmann_json)
create_target_directory_groups(mcp)
