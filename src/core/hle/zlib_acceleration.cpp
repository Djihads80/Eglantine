// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Host replacement for a guest zlib inflate_fast. The decoding loop follows zlib's inffast.c
// (Copyright (C) 1995-2017 Mark Adler, zlib license), adapted to work on copies of guest buffers.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "common/logging.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/svc_types.h"
#include "core/hle/zlib_acceleration.h"
#include "core/memory.h"

namespace Core::HLE::Zlib {
namespace {

// z_stream and inflate_state offsets for zlib 1.2.12+ built for AArch64 (LP64).
constexpr u64 StrmNextIn = 0x00;
constexpr u64 StrmAvailIn = 0x08;
constexpr u64 StrmNextOut = 0x18;
constexpr u64 StrmAvailOut = 0x20;
constexpr u64 StrmMsg = 0x30;
constexpr u64 StrmState = 0x38;
constexpr u64 StateMode = 0x08;
constexpr u64 StateWsize = 0x3c;
constexpr u64 StateWhave = 0x40;
constexpr u64 StateWnext = 0x44;
constexpr u64 StateWindow = 0x48;
constexpr u64 StateHold = 0x50;
constexpr u64 StateBits = 0x58;
constexpr u64 StateLencode = 0x68;
constexpr u64 StateDistcode = 0x70;
constexpr u64 StateLenbits = 0x78;
constexpr u64 StateDistbits = 0x7c;
constexpr u64 StateSane = 0x1be8;

constexpr u32 ModeType = 16191;
constexpr u32 ModeBad = 16209;

// Table sizes from inftrees.h (ENOUGH_LENS / ENOUGH_DISTS).
constexpr size_t LenCodes = 852;
constexpr size_t DistCodes = 592;
// Deflate distances never exceed 32 KiB; larger calls are split by inflate() re-entering.
constexpr size_t MaxHistory = 32768;
constexpr size_t InputCap = 64 * 1024;
constexpr size_t OutputCap = 512 * 1024;
// Approximate guest instructions the emulated loop spends per decoded symbol and per output byte,
// charged so single-core time slices still end where the emulated version would have.
constexpr u64 GuestInstructionsPerSymbol = 14;
constexpr u64 GuestInstructionsPerByte = 1;

struct Code {
    u8 op;
    u8 bits;
    u16 val;
};
static_assert(sizeof(Code) == 4);

enum class Message { TooFarBack, BadDistance, BadLength };

struct Hook {
    std::array<u64, 3> messages; // guest addresses, indexed by Message
};

std::mutex hooks_mutex;
std::unordered_map<u64, Hook> hooks;

// Instructions at the start of the recognised build: pacibsp, frame setup, then the first loads
// of strm->avail_in and strm->next_in. ADRP/ADD pairs that load the error strings follow.
constexpr std::array<u32, 12> Prologue{
    0xd503237f, 0xd10343ff, 0xa9077bfd, 0xa9086ffc, 0xa90967fa, 0xa90a5ff8,
    0xa90b57f6, 0xa90c4ff4, 0xb9400808, 0x1280000d, 0x2a2103eb, 0xf940000c,
};
constexpr u32 LoadStateInstruction = 0xf9401c08; // ldr x8, [x0, #0x38]
constexpr size_t LoadStateOffset = 0x5c;
constexpr size_t DistanceCodeAdrp = 0x30;   // x21: "invalid distance code"
constexpr size_t TooFarBackAdrp = 0x3c;     // x18: "invalid distance too far back"
constexpr size_t LengthCodeAdrp = 0xed8;    // x18: "invalid literal/length code"

std::optional<u64> DecodeAdrpAdd(std::span<const u32> code, u64 base, size_t offset, u32 reg) {
    const size_t index = offset / 4;
    if (index + 1 >= code.size()) {
        return std::nullopt;
    }
    const u32 adrp = code[index];
    const u32 add = code[index + 1];
    if ((adrp & 0x9f00001f) != (0x90000000 | reg) || (add & 0xffc003ff) != (0x91000000 | (reg << 5) | reg)) {
        return std::nullopt;
    }
    const s64 imm = static_cast<s64>(((adrp >> 5) & 0x7ffff) << 2 | ((adrp >> 29) & 3)) << 43 >> 31;
    const u64 page = ((base + offset) & ~u64{0xfff}) + static_cast<u64>(imm);
    return page + ((add >> 10) & 0xfff);
}

bool Disabled() {
    static const bool disabled = std::getenv("EDEN_DISABLE_ZLIB_HLE") != nullptr;
    return disabled;
}

struct Buffers {
    std::vector<u8> input;
    std::vector<u8> output;
    std::vector<u8> window;
    std::array<Code, LenCodes> lcode;
    std::array<Code, DistCodes> dcode;
};

} // namespace

void Install(Kernel::KProcess& process, u64 base, u64 size) {
    if (Disabled() || size < Prologue.size() * 4) {
        return;
    }
    auto& memory = process.GetMemory();
    std::vector<u32> code(size / 4);
    if (!memory.ReadBlock(base, code.data(), code.size() * 4)) {
        return;
    }
    for (size_t i = 0; i + Prologue.size() <= code.size(); ++i) {
        if (code[i] != Prologue[0] || !std::equal(Prologue.begin(), Prologue.end(), code.begin() + i)) {
            continue;
        }
        const u64 entry = base + i * 4;
        const std::span<const u32> function{code.data() + i, code.size() - i};
        const auto distance = DecodeAdrpAdd(function, entry, DistanceCodeAdrp, 21);
        const auto too_far = DecodeAdrpAdd(function, entry, TooFarBackAdrp, 18);
        const auto length = DecodeAdrpAdd(function, entry, LengthCodeAdrp, 18);
        if (function.size() <= LoadStateOffset / 4 || function[LoadStateOffset / 4] != LoadStateInstruction ||
            !distance || !too_far || !length) {
            LOG_WARNING(HW_Memory, "zlib inflate_fast candidate at {:#x} does not match the known layout", entry);
            continue;
        }
        {
            std::scoped_lock lock{hooks_mutex};
            hooks[entry] = Hook{{*too_far, *distance, *length}};
        }
        memory.Write32(entry, 0xd4000001 | (InflateFastSvc << 5));
        LOG_INFO(HW_Memory, "Accelerating guest zlib inflate_fast at {:#x}", entry);
    }
}

bool HandleSvc(Core::System& system, u32 imm, std::span<u64, 8> args) {
    if (imm != InflateFastSvc) {
        return false;
    }
    auto& kernel = system.Kernel();
    auto& process = Kernel::GetCurrentProcess(kernel);
    auto& memory = process.GetMemory();
    auto* const arm = process.GetArmInterface(kernel.CurrentPhysicalCore().CoreIndex());

    Kernel::Svc::ThreadContext ctx{};
    arm->GetContext(ctx);
    const u64 entry = ctx.pc - 4;
    Hook hook{};
    {
        std::scoped_lock lock{hooks_mutex};
        const auto it = hooks.find(entry);
        if (it == hooks.end()) {
            LOG_CRITICAL(HW_Memory, "zlib SVC from unknown address {:#x}", entry);
            return true;
        }
        hook = it->second;
    }

    const u64 strm = args[0];
    const u32 start = static_cast<u32>(args[1]);
    const u64 state = memory.Read64(strm + StrmState);
    const u64 next_in = memory.Read64(strm + StrmNextIn);
    const u32 avail_in = memory.Read32(strm + StrmAvailIn);
    const u64 next_out = memory.Read64(strm + StrmNextOut);
    const u32 avail_out = memory.Read32(strm + StrmAvailOut);
    const u32 wsize = memory.Read32(state + StateWsize);
    const u32 whave = memory.Read32(state + StateWhave);
    const u32 wnext = memory.Read32(state + StateWnext);
    const u64 window_addr = memory.Read64(state + StateWindow);
    u64 hold = memory.Read64(state + StateHold);
    u32 bits = memory.Read32(state + StateBits);
    const u64 lencode = memory.Read64(state + StateLencode);
    const u64 distcode = memory.Read64(state + StateDistcode);
    const u32 lenbits = memory.Read32(state + StateLenbits);
    const u32 distbits = memory.Read32(state + StateDistbits);
    const bool sane = memory.Read32(state + StateSane) != 0;

    thread_local Buffers buf;
    const size_t in_len = std::min<size_t>(avail_in, InputCap);
    const size_t out_len = std::min<size_t>(avail_out, OutputCap);
    const size_t produced_before = start - avail_out; // bytes inflate() already wrote this call
    const size_t history = std::min(produced_before, MaxHistory);
    buf.input.resize(in_len);
    buf.output.resize(history + out_len);
    buf.window.resize(wsize);
    memory.ReadBlock(next_in, buf.input.data(), in_len);
    memory.ReadBlock(next_out - history, buf.output.data(), history);
    if (window_addr && wsize) {
        memory.ReadBlock(window_addr, buf.window.data(), wsize);
    }
    memory.ReadBlock(lencode, buf.lcode.data(), sizeof(buf.lcode));
    memory.ReadBlock(distcode, buf.dcode.data(), sizeof(buf.dcode));

    const u8* in = buf.input.data();
    const u8* const last = in + (in_len - 5);
    u8* const out_start = buf.output.data() + history;
    u8* out = out_start;
    u8* const end = out + (out_len - 257);
    const u8* const window = buf.window.data();
    const u32 lmask = (1U << lenbits) - 1;
    const u32 dmask = (1U << distbits) - 1;
    std::optional<u32> new_mode;
    std::optional<Message> error;
    // Distances are checked against everything inflate() has written, not just the history copy.
    const auto written = [&] { return static_cast<u32>(produced_before + (out - out_start)); };

    u64 symbols = 0;
    do {
        ++symbols;
        if (bits < 15) {
            hold += u64{*in++} << bits;
            bits += 8;
            hold += u64{*in++} << bits;
            bits += 8;
        }
        u32 index = static_cast<u32>(hold & lmask);
        Code here = buf.lcode[index];
        u32 op;
        for (;;) {
            op = here.bits;
            hold >>= op;
            bits -= op;
            op = here.op;
            if (op == 0 || (op & 16) || (op & 64)) {
                break;
            }
            index = here.val + static_cast<u32>(hold & ((1U << op) - 1));
            if (index >= LenCodes) {
                op = 64;
                break;
            }
            here = buf.lcode[index];
        }
        if (op == 0) {
            *out++ = static_cast<u8>(here.val);
        } else if (op & 16) {
            u32 len = here.val;
            op &= 15;
            if (op) {
                if (bits < op) {
                    hold += u64{*in++} << bits;
                    bits += 8;
                }
                len += static_cast<u32>(hold) & ((1U << op) - 1);
                hold >>= op;
                bits -= op;
            }
            if (bits < 15) {
                hold += u64{*in++} << bits;
                bits += 8;
                hold += u64{*in++} << bits;
                bits += 8;
            }
            index = static_cast<u32>(hold & dmask);
            here = buf.dcode[index];
            for (;;) {
                op = here.bits;
                hold >>= op;
                bits -= op;
                op = here.op;
                if ((op & 16) || (op & 64)) {
                    break;
                }
                index = here.val + static_cast<u32>(hold & ((1U << op) - 1));
                if (index >= DistCodes) {
                    op = 64;
                    break;
                }
                here = buf.dcode[index];
            }
            if (!(op & 16)) {
                error = Message::BadDistance;
                break;
            }
            u32 dist = here.val;
            op &= 15;
            if (bits < op) {
                hold += u64{*in++} << bits;
                bits += 8;
                if (bits < op) {
                    hold += u64{*in++} << bits;
                    bits += 8;
                }
            }
            dist += static_cast<u32>(hold) & ((1U << op) - 1);
            hold >>= op;
            bits -= op;
            op = written();
            if (dist > op) {
                op = dist - op;
                if (op > whave && sane) {
                    error = Message::TooFarBack;
                    break;
                }
                if (op > wsize) {
                    error = Message::TooFarBack;
                    break;
                }
                const u8* from = window;
                if (wnext == 0) {
                    from += wsize - op;
                    if (op < len) {
                        len -= op;
                        do {
                            *out++ = *from++;
                        } while (--op);
                        from = out - dist;
                    }
                } else if (wnext < op) {
                    from += wsize + wnext - op;
                    op -= wnext;
                    if (op < len) {
                        len -= op;
                        do {
                            *out++ = *from++;
                        } while (--op);
                        from = window;
                        if (wnext < len) {
                            op = wnext;
                            len -= op;
                            do {
                                *out++ = *from++;
                            } while (--op);
                            from = out - dist;
                        }
                    }
                } else {
                    from += wnext - op;
                    if (op < len) {
                        len -= op;
                        do {
                            *out++ = *from++;
                        } while (--op);
                        from = out - dist;
                    }
                }
                while (len > 2) {
                    *out++ = *from++;
                    *out++ = *from++;
                    *out++ = *from++;
                    len -= 3;
                }
                if (len) {
                    *out++ = *from++;
                    if (len > 1) {
                        *out++ = *from++;
                    }
                }
            } else {
                if (dist > static_cast<u32>(out - buf.output.data())) {
                    // Only possible with a corrupt stream: the copy would precede the history.
                    error = Message::TooFarBack;
                    break;
                }
                const u8* from = out - dist;
                do {
                    *out++ = *from++;
                    *out++ = *from++;
                    *out++ = *from++;
                    len -= 3;
                } while (len > 2);
                if (len) {
                    *out++ = *from++;
                    if (len > 1) {
                        *out++ = *from++;
                    }
                }
            }
        } else if (op & 32) {
            new_mode = ModeType;
            break;
        } else {
            error = Message::BadLength;
            break;
        }
    } while (in < last && out < end);

    // Return unused whole bytes; on entry bits < 8, so this never rewinds past the start.
    const u32 unused = bits >> 3;
    in -= unused;
    bits -= unused << 3;
    hold &= (1U << bits) - 1;

    const u32 consumed = static_cast<u32>(in - buf.input.data());
    const u32 produced = static_cast<u32>(out - out_start);
    memory.WriteBlock(next_out, out_start, produced);
    memory.Write64(strm + StrmNextIn, next_in + consumed);
    memory.Write32(strm + StrmAvailIn, avail_in - consumed);
    memory.Write64(strm + StrmNextOut, next_out + produced);
    memory.Write32(strm + StrmAvailOut, avail_out - produced);
    memory.Write64(state + StateHold, hold);
    memory.Write32(state + StateBits, bits);
    if (error) {
        memory.Write64(strm + StrmMsg, hook.messages[static_cast<size_t>(*error)]);
        new_mode = ModeBad;
    }
    if (new_mode) {
        memory.Write32(state + StateMode, *new_mode);
    }

    if (!kernel.IsMulticore()) {
        const u64 instructions = symbols * GuestInstructionsPerSymbol + produced * GuestInstructionsPerByte;
        system.CoreTiming().AddTicks(std::max<u64>(instructions / Core::Hardware::NUM_CPU_CORES, 1));
    }

    // Return to the caller as the guest function's retab would.
    ctx.pc = ctx.lr;
    arm->SetContext(ctx);
    return true;
}

} // namespace Core::HLE::Zlib
