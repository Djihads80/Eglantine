// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <span>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/uniform_stream_reuse.h"

namespace {
void Check(bool condition, const char* message) {
    INFO(message);
    REQUIRE(condition);
}

struct Memory {
    std::array<u8, 8192> bytes{};
    void ReadBlockUnsafe(u64 address, void* destination, u32 size) {
        Check(address + size <= bytes.size(), "read outside guest memory");
        std::memcpy(destination, bytes.data() + address, size);
    }
};

struct Runtime {
    struct StreamUniformRef {
        size_t allocation;
    };
    std::vector<std::vector<u8>> allocations;
    StreamUniformRef last{}, bound{};
    u64 tick{1};
    std::function<void()> on_allocate;
    u64 CurrentTick() const {
        return tick;
    }
    StreamUniformRef LastStreamUniformRef() const {
        return last;
    }
    std::span<u8> BindMappedUniformBuffer(size_t, u32, u32 size) {
        allocations.emplace_back(size);
        last = bound = StreamUniformRef{allocations.size() - 1};
        if (on_allocate) {
            on_allocate();
        }
        return allocations.back();
    }
    void RebindStreamUniform(StreamUniformRef ref, u32 size) {
        Check(ref.allocation < allocations.size(), "reused a recycled allocation");
        Check(allocations[ref.allocation].size() == size, "reused wrong allocation size");
        bound = ref;
    }
    void NextTick() {
        ++tick;
        allocations.clear();
    }
};

struct DirectMemory : Memory {
    size_t pointer_reads{};
    size_t block_reads{};
    bool mapped{true};

    template <typename T>
    T* GetPointer(u64 address) {
        ++pointer_reads;
        Check(address < bytes.size(), "pointer outside guest memory");
        return mapped ? reinterpret_cast<T*>(bytes.data() + address) : nullptr;
    }

    void ReadBlockUnsafe(u64 address, void* destination, u32 size) {
        ++block_reads;
        if (mapped) {
            Memory::ReadBlockUnsafe(address, destination, size);
        } else {
            Check(address + size <= bytes.size(), "read outside guest memory");
            std::memset(destination, 0, size);
        }
    }
};

void CheckBound(const Runtime& runtime, const Memory& memory, u64 address, u32 size) {
    const auto& bytes = runtime.allocations.at(runtime.bound.allocation);
    Check(bytes.size() == size &&
              std::memcmp(bytes.data(), memory.bytes.data() + address, size) == 0,
          "bound uniform data differs from guest memory");
}
} // namespace

TEST_CASE("Uniform stream reuse preserves data through mutation and session resets",
          "[video_core][uniform_reuse]") {
    using VideoCommon::BindSmallUniformReusing;
    Memory memory;
    Runtime runtime;
    VideoCommon::ResetUniformStreamReuse<Runtime>();
    auto bind = [&](size_t stage, u32 slot, u64 address, u32 size) {
        Check(BindSmallUniformReusing(runtime, memory, stage, slot, slot, address, size),
              "valid small uniform rejected");
        CheckBound(runtime, memory, address, size);
    };
    bind(0, 0, 0, 64);
    bind(0, 0, 0, 64);
    Check(runtime.allocations.size() == 1, "identical bytes were copied twice");
    memory.bytes[7] = 23;
    bind(0, 0, 0, 64);
    Check(runtime.allocations.size() == 2, "changed bytes reused stale data");
    bind(0, 1, 0, 64);
    bind(1, 0, 0, 64);
    Check(runtime.allocations.size() == 4, "stage or slot shared the wrong allocation");
    runtime.NextTick();
    bind(0, 0, 0, 64);
    Check(runtime.allocations.size() == 1, "a new command buffer reused an old allocation");
    VideoCommon::ResetUniformStreamReuse<Runtime>();
    bind(0, 0, 0, 64);
    Check(runtime.allocations.size() == 2, "session reset retained an old allocation");
    bind(4, 17, 0, 4096);
    const auto before = runtime.allocations.size();
    for (auto [stage, slot, size] :
         {std::array<u32, 3>{5, 0, 64}, {0, 18, 64}, {0, 0, 0}, {0, 0, 4097}}) {
        Check(!BindSmallUniformReusing(runtime, memory, stage, slot, slot, 0, size),
              "invalid uniform range did not use the fallback");
    }
    Check(runtime.allocations.size() == before, "fallback allocated stream memory");

    u32 random = 17;
    for (u32 index = 0; index < 10000; ++index) {
        random = random * 1664525U + 1013904223U;
        if (index % 37 == 0)
            runtime.NextTick();
        if (index % 503 == 0)
            VideoCommon::ResetUniformStreamReuse<Runtime>();
        const u32 size = 1 + random % 4096;
        const u64 address = (random >> 12) % (memory.bytes.size() - 4096);
        if (index % 3 == 0)
            memory.bytes[address + random % size] ^= u8{0x5a};
        const size_t stage = (random >> 20) % 5;
        const u32 slot = (random >> 24) % 18;
        bind(stage, slot, address, size);
        bind(stage, slot, address, size);
    }
}

TEST_CASE("Uniform reuse compares current mapped bytes and survives tick and session changes",
          "[video_core][uniform_reuse]") {
    DirectMemory memory;
    Runtime runtime;
    VideoCommon::ResetUniformStreamReuse<Runtime>();
    const auto bind = [&](u64 address, u32 size) {
        REQUIRE(VideoCommon::BindSmallUniformReusing(runtime, memory, 0, 0, 0, address, size));
        CheckBound(runtime, memory, address, size);
    };
    bind(128, 64);
    bind(128, 64);
    CHECK(runtime.allocations.size() == 1);
    CHECK(memory.block_reads == 0);
    bind(256, 64); // Same bytes at another address may reuse the allocation.
    CHECK(runtime.allocations.size() == 1);
    memory.bytes[257] = 0x37;
    bind(256, 64); // Same address cannot bypass the byte comparison.
    CHECK(runtime.allocations.size() == 2);
    runtime.NextTick();
    bind(256, 64);
    CHECK(runtime.allocations.size() == 1);
    VideoCommon::ResetUniformStreamReuse<Runtime>();
    bind(256, 64);
    CHECK(runtime.allocations.size() == 2);
    bind(0, 4096); // Exactly one full page is eligible.
    CHECK(memory.pointer_reads == 7);
    CHECK(memory.block_reads == 0);
}

TEST_CASE("Uniform reuse keeps block-reader fallback at page boundaries and unmapped memory",
          "[video_core][uniform_reuse]") {
    DirectMemory memory;
    Runtime runtime;
    VideoCommon::ResetUniformStreamReuse<Runtime>();
    memory.bytes[4095] = 0x31;
    memory.bytes[4096] = 0x72;
    REQUIRE(VideoCommon::BindSmallUniformReusing(runtime, memory, 0, 0, 0, 4095, 2));
    CheckBound(runtime, memory, 4095, 2);
    CHECK(memory.pointer_reads == 0);
    CHECK(memory.block_reads == 1);
    memory.mapped = false;
    REQUIRE(VideoCommon::BindSmallUniformReusing(runtime, memory, 0, 0, 0, 64, 16));
    CHECK(memory.pointer_reads == 1);
    CHECK(memory.block_reads == 2);
    CHECK(std::ranges::all_of(runtime.allocations.at(runtime.bound.allocation),
                             [](u8 byte) { return byte == 0; }));
    memory.mapped = true;
    memory.bytes[64] = 0x89;
    REQUIRE(VideoCommon::BindSmallUniformReusing(runtime, memory, 0, 0, 0, 64, 16));
    CheckBound(runtime, memory, 64, 16);
    CHECK(memory.block_reads == 2);
}

TEST_CASE("Uniform reuse snapshots bytes before a staging allocation changes guest memory",
          "[video_core][uniform_reuse]") {
    DirectMemory memory;
    Runtime runtime;
    VideoCommon::ResetUniformStreamReuse<Runtime>();
    std::fill_n(memory.bytes.begin() + 64, 16, 0x24);
    runtime.on_allocate = [&] { std::fill_n(memory.bytes.begin() + 64, 16, 0x69); };
    REQUIRE(VideoCommon::BindSmallUniformReusing(runtime, memory, 0, 0, 0, 64, 16));
    CHECK(std::ranges::all_of(runtime.allocations.at(runtime.bound.allocation),
                             [](u8 byte) { return byte == 0x24; }));
    runtime.on_allocate = {};
    REQUIRE(VideoCommon::BindSmallUniformReusing(runtime, memory, 0, 0, 0, 64, 16));
    CheckBound(runtime, memory, 64, 16);
    CHECK(runtime.allocations.size() == 2);
    REQUIRE(VideoCommon::BindSmallUniformReusing(runtime, memory, 0, 0, 0, 64, 16));
    CHECK(runtime.allocations.size() == 2);
}
