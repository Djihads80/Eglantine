# SPDX-FileCopyrightText: Copyright 2025 crueter
# SPDX-License-Identifier: LGPL-3.0-or-later

## FasterLinker ##

# This finds a faster linker for your compiler, if available.
# Only really tested on Linux. I would not recommend this on MSYS2.

#[[
    search order:
    - gold (GCC only) - the best, generally, but not packaged anymore
    - mold is no longer supported, that thing is slower than the pre-Ventura linker lmao
    - lld - preferred on clang
    - bfd - the final fallback
    - If none are found just use the default linker
]]

# This module is based on the work of Yuzu, specifically Liam White,
# and later extended by crueter.

option(USE_FASTER_LINKER "Attempt to use a faster linker program" OFF)

if (USE_FASTER_LINKER)
    macro(find_linker ld)
        find_program(LINKER_${ld} ld.${ld})
        if (LINKER_${ld})
            set(LINKER ${ld})
        endif()
    endmacro()

    find_linker(bfd)
    find_linker(lld)

    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        find_linker(gold)

        if (LINKER STREQUAL "lld")
            message(WARNING
                "[FasterLinker] Using lld on GCC may cause issues.\
                 Install mold, gold, or disable USE_FASTER_LINKER.")
        endif()
    endif()

    if (LINKER)
        message(NOTICE "[FasterLinker] Selecting ${LINKER} as linker")
        add_link_options("-fuse-ld=${LINKER}")
    else()
        message(WARNING "[FasterLinker] No faster linker found--using default")
    endif()
endif()
