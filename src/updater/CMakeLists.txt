# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(eglantine-updater main.cpp ../frontend_common/update_installer.cpp)
target_include_directories(eglantine-updater PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(eglantine-updater PRIVATE cxx_std_20)
target_link_libraries(eglantine-updater PRIVATE user32 shell32)
set_target_properties(eglantine-updater PROPERTIES WIN32_EXECUTABLE ON)

# The helper runs from a temporary folder, away from the DLLs next to eglantine.exe, so it links the
# C runtime statically. The top-level CMakeLists appends /MD (or /MDd) to every target's options, so
# the flag is set as a target option, which comes after those.
if (MSVC)
    target_compile_options(eglantine-updater PRIVATE
        "$<IF:$<CONFIG:Debug>,/MTd,/MT>")
else()
    target_link_options(eglantine-updater PRIVATE -static)
endif()
