// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "video_core/macro_turing_jit_arm64.h"

namespace Tegra {

void* TuringMacroJit::AllocateCode(std::size_t bytes) {
#ifdef _WIN32
    return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void* const memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? nullptr : memory;
#endif
}

bool TuringMacroJit::SealCode(void* memory, std::size_t bytes) {
#ifdef _WIN32
    DWORD old_protection{};
    if (!VirtualProtect(memory, bytes, PAGE_EXECUTE_READ, &old_protection)) {
        return false;
    }
    return FlushInstructionCache(GetCurrentProcess(), memory, bytes) != 0;
#else
    if (mprotect(memory, bytes, PROT_READ | PROT_EXEC) != 0) {
        return false;
    }
    char* const begin = static_cast<char*>(memory);
    __builtin___clear_cache(begin, begin + bytes);
    return true;
#endif
}

void TuringMacroJit::FreeCode(void* memory, std::size_t bytes) {
#ifdef _WIN32
    VirtualFree(memory, 0, MEM_RELEASE);
#else
    munmap(memory, bytes);
#endif
}

} // namespace Tegra
