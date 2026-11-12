# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2023 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_library(frontend_common STATIC
    config.cpp
    config.h
    content_manager.h
    firmware_manager.h
    firmware_manager.cpp
    data_manager.h data_manager.cpp
    play_time_manager.cpp
    play_time_manager.h
    settings_generator.h settings_generator.cpp
    mod_manager.h mod_manager.cpp)

if (ENABLE_UPDATE_CHECKER)
    # Embed tools/tester-build/recipe.json for branches that predate the file. The tests target
    # compiles the same generated source.
    set(TESTER_RECIPE_JSON_FILE ${PROJECT_SOURCE_DIR}/tools/tester-build/recipe.json)
    set(TESTER_RECIPE_SOURCE ${CMAKE_BINARY_DIR}/src/frontend_common/tester_recipe.cpp)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TESTER_RECIPE_JSON_FILE})
    file(READ ${TESTER_RECIPE_JSON_FILE} TESTER_RECIPE_JSON)
    # Adjacent literals, because MSVC limits a single literal to about 16 KB.
    set(TESTER_RECIPE_LITERAL "")
    string(LENGTH "${TESTER_RECIPE_JSON}" TESTER_RECIPE_LENGTH)
    set(TESTER_RECIPE_OFFSET 0)
    while (TESTER_RECIPE_OFFSET LESS TESTER_RECIPE_LENGTH)
        string(SUBSTRING "${TESTER_RECIPE_JSON}" ${TESTER_RECIPE_OFFSET} 8000 TESTER_RECIPE_CHUNK)
        string(APPEND TESTER_RECIPE_LITERAL "R\"recipe(${TESTER_RECIPE_CHUNK})recipe\"
")
        math(EXPR TESTER_RECIPE_OFFSET "${TESTER_RECIPE_OFFSET} + 8000")
    endwhile()
    file(CONFIGURE OUTPUT ${TESTER_RECIPE_SOURCE} CONTENT
"// Generated from tools/tester-build/recipe.json by CMake.
#include <string_view>
#include \"frontend_common/tester_build.h\"

std::string_view TesterBuild::EmbeddedRecipe() {
    return std::string_view{
@TESTER_RECIPE_LITERAL@    };
}
" @ONLY)

    target_link_libraries(frontend_common PRIVATE httplib::httplib)
    target_link_libraries(frontend_common PRIVATE nlohmann_json::nlohmann_json)
    target_sources(frontend_common PRIVATE
        update_checker.cpp
        update_checker.h
        update_installer.cpp
        update_installer.h
        tester_build.cpp
        tester_build.h
        ${TESTER_RECIPE_SOURCE})
    target_link_libraries(frontend_common PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

create_target_directory_groups(frontend_common)
target_link_libraries(frontend_common
    PUBLIC core SimpleIni::SimpleIni frozen::frozen-headers
    PRIVATE common Boost::headers)
