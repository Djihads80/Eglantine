// Synthetic VRAM churn through VMA's own block metadata: how much slack (block space no allocation
// uses) different block sizes, strategies and dedicated-allocation thresholds leave.
//
// build (header-only VMA from the CPM cache; run from the repository root):
//   g++ -std=c++20 -O2 -w -I .cache/cpm/vulkanmemoryallocator/v3.3.0/include \
//       -o vma_churn_sim tools/nx2/vma_churn_sim.cpp
// usage: ./vma_churn_sim [working set MiB (12000)] [eviction rounds (300)] [evicted share (0.10)]
//
// The workload is an assumption, not a trace: texture sizes log-uniform in area from 32x32 to
// 4096x2048 texels, 25% BC1 / 60% BC3 / 15% RGBA8 with a third more for mips, 64 KiB alignment;
// each round evicts about 10% of the live bytes (third argument) at random and refills. Blocks are tried fullest
// first, as VmaBlockVector keeps them, then a new block is made. Allocations over half a block
// are dedicated, as VMA does. Compare the options against each other; the absolute numbers only
// matter if the size mix matches a real trace (12 GB working set: about 2.8 GiB of slack for 64 MiB
// blocks here, against the 1-3 GiB measured in tester logs with tools/nx2/vma_slack.py).
// Synthetic VRAM churn through VMA's TLSF block metadata (virtual blocks), comparing strategies.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vulkan/vulkan.h>
#include "vk_mem_alloc.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <random>
#include <vector>
using u64 = unsigned long long;
struct Live { size_t block; VmaVirtualAllocation a; u64 size; bool dedicated; };
struct Cfg { const char* name; u64 block; uint32_t strategy; u64 dedicated_min; };
static u64 sample_size(std::mt19937_64& g) {
    std::uniform_real_distribution<double> u(0, 1);
    // texture area log-uniform 32x32 .. 4096x2048 texels, bytes/texel from a BC1/BC3/RGBA8 mix, +33% mips
    const double area = std::exp(std::log(1024.0) + u(g) * (std::log(4096.0 * 2048.0) - std::log(1024.0)));
    const double r = u(g);
    const double bpt = r < 0.25 ? 0.5 : r < 0.85 ? 1.0 : 4.0;
    u64 s = (u64)(area * bpt * 1.33);
    s = (s + 65535) & ~65535ULL; // 64 KiB alignment
    return std::max<u64>(s, 65536);
}
static double g_evict = 0.10; // fraction of the live bytes evicted per round
static void run(const Cfg& c, u64 working_set, int churn_rounds, unsigned seed) {
    std::mt19937_64 g(seed);
    std::vector<VmaVirtualBlock> blocks;
    std::vector<u64> used; // allocated bytes per block
    std::vector<Live> live;
    u64 dedicated_bytes = 0, live_bytes = 0, failures = 0, dedicated_live = 0, dedicated_allocs = 0, rounds_done = 0;
    auto alloc_one = [&]() {
        const u64 size = sample_size(g);
        if (size >= c.dedicated_min || size > c.block / 2) { // VMA also dedicates above block/2
            live.push_back({~size_t(0), nullptr, size, true}); dedicated_bytes += size; live_bytes += size; ++dedicated_live; ++dedicated_allocs; return;
        }
        // blocks ordered by free space ascending (fullest first), as VmaBlockVector keeps them
        std::vector<size_t> order(blocks.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return used[a] > used[b]; });
        for (size_t i : order) {
            VmaVirtualAllocationCreateInfo ci{}; ci.size = size; ci.alignment = 65536; ci.flags = c.strategy;
            VmaVirtualAllocation a; VkDeviceSize off;
            if (vmaVirtualAllocate(blocks[i], &ci, &a, &off) == VK_SUCCESS) {
                used[i] += size; live.push_back({i, a, size, false}); live_bytes += size; return;
            }
        }
        VmaVirtualBlockCreateInfo bi{}; bi.size = c.block; VmaVirtualBlock b; vmaCreateVirtualBlock(&bi, &b);
        blocks.push_back(b); used.push_back(0);
        VmaVirtualAllocationCreateInfo ci{}; ci.size = size; ci.alignment = 65536; ci.flags = c.strategy;
        VmaVirtualAllocation a; VkDeviceSize off;
        if (vmaVirtualAllocate(b, &ci, &a, &off) != VK_SUCCESS) { ++failures; return; }
        used.back() += size; live.push_back({blocks.size() - 1, a, size, false}); live_bytes += size;
    };
    auto free_one = [&](size_t idx) {
        Live l = live[idx]; live[idx] = live.back(); live.pop_back();
        live_bytes -= l.size;
        if (l.dedicated) { dedicated_bytes -= l.size; --dedicated_live; return; }
        vmaVirtualFree(blocks[l.block], l.a); used[l.block] -= l.size;
    };
    while (live_bytes < working_set) alloc_one();
    std::vector<double> samples;
    for (int round = 0; round < churn_rounds; ++round) { ++rounds_done;
        // evict ~10% of the live bytes (random victims), then refill to the working set
        const u64 target = working_set - (u64)(working_set * g_evict);
        while (live_bytes > target && !live.empty()) free_one(g() % live.size());
        while (live_bytes < working_set) alloc_one();
        if (round >= churn_rounds / 2) {
            u64 blocks_bytes = 0; // empty blocks are freed by VMA (keeps one)
            size_t nonempty = 0;
            for (size_t i = 0; i < blocks.size(); ++i) if (used[i]) { blocks_bytes += c.block; ++nonempty; }
            const double slack = double(blocks_bytes) - double(live_bytes - dedicated_bytes);
            samples.push_back(slack);
        }
    }
    std::sort(samples.begin(), samples.end());
    auto q = [&](double p) { return samples[size_t(p * (samples.size() - 1))] / (1 << 20); };
    u64 blocks_bytes = 0; for (size_t i = 0; i < blocks.size(); ++i) if (used[i]) blocks_bytes += c.block;
    std::printf("%-34s slack MiB p50 %6.0f  p90 %6.0f  max %6.0f   of blocks %6.0f MiB   dedicated %5.0f MiB (%zu live, %llu fails)  blocks %zu + dedicated %llu allocations; %llu dedicated allocs/round\n",
                c.name, q(0.5), q(0.9), q(1.0), blocks_bytes / 1048576.0, dedicated_bytes / 1048576.0, live.size(), failures, (size_t)(blocks_bytes / c.block), dedicated_live, dedicated_allocs / std::max<u64>(rounds_done, 1));

}
int main(int argc, char** argv) {
    const u64 W = (argc > 1 ? std::atoll(argv[1]) : 12000) * 1048576ULL;
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 300;
    if (argc > 3) g_evict = std::atof(argv[3]);
    const u64 MiB = 1048576ULL, none = ~0ULL;
    Cfg cfgs[] = {
        {"64 MiB blocks, default (today)", 64 * MiB, 0, none},
        {"64 MiB blocks, MIN_MEMORY", 64 * MiB, VMA_VIRTUAL_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT, none},
        {"64 MiB blocks, MIN_OFFSET", 64 * MiB, VMA_VIRTUAL_ALLOCATION_CREATE_STRATEGY_MIN_OFFSET_BIT, none},
        {"32 MiB blocks, default", 32 * MiB, 0, none},
        {"16 MiB blocks, default", 16 * MiB, 0, none},
        {"256 MiB blocks, default (was)", 256 * MiB, 0, none},
        {"64 MiB, default, dedicated >=16 MiB", 64 * MiB, 0, 16 * MiB},
        {"64 MiB, default, dedicated >=8 MiB", 64 * MiB, 0, 8 * MiB},
        {"64 MiB, MIN_MEMORY, ded >=16 MiB", 64 * MiB, VMA_VIRTUAL_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT, 16 * MiB},
    };
    for (auto& c : cfgs) run(c, W, rounds, 12345);
}
