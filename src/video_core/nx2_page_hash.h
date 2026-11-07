// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstring>

#include "common/common_types.h"

#ifdef ARCHITECTURE_x86_64
#include <immintrin.h>
#include "common/cpu_features.h"
#if defined(__GNUC__) || defined(__clang__)
#define NX2_PAGE_HASH_AVX2 __attribute__((target("avx2")))
#else
#define NX2_PAGE_HASH_AVX2
#endif
#endif

// bank_hash_shadow: a 64-bit change-detection hash of a native uniform bank page. Four u64
// lanes over 32-byte stripes (XXH3-style 32x32 multiply accumulate, keys that change per
// stripe). A changed u64 word always changes its neighbour lane's sum, so a single-word edit
// never collides. Not for hash tables or anything adversarial. Every path computes the same value.
namespace VideoCommon::Nx2PageHash {

namespace detail {

inline constexpr std::array<u64, 4> keys{0x9E3779B185EBCA87ULL, 0xC2B2AE3D27D4EB4FULL,
                                         0x165667B19E3779F9ULL, 0x85EBCA77C2B2AE63ULL};
inline constexpr std::array<u64, 4> acc_init{keys[2], keys[3], keys[0], keys[1]};
inline constexpr u64 key_step = 0x27D4EB2F165667C5ULL;
inline constexpr u64 prime = 0x9E3779B97F4A7C15ULL;

inline u64 Load64(const u8* ptr) {
    u64 value;
    std::memcpy(&value, ptr, sizeof(value));
    return value;
}

inline void Accumulate(std::array<u64, 4>& acc, u64 word, u64 key, size_t lane) {
    const u64 mixed = word ^ key;
    acc[lane ^ 1] += word;
    acc[lane] += (mixed & 0xFFFFFFFFULL) * (mixed >> 32);
}

/// Bijective 64-bit finalizer (murmur3 fmix64).
inline u64 Avalanche(u64 x) {
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ULL;
    x ^= x >> 33;
    return x;
}

/// The words after the whole stripes (a partial word is zero-padded), then the final mix.
inline u64 Finish(std::array<u64, 4>& acc, const u8* data, size_t size) {
    const size_t stripes = size / 32;
    const u64 tail_key = stripes * key_step;
    size_t offset = stripes * 32;
    size_t lane = 0;
    for (; size - offset >= 8; offset += 8, ++lane) {
        Accumulate(acc, Load64(data + offset), keys[lane] + tail_key, lane);
    }
    if (offset < size) {
        u64 word = 0;
        std::memcpy(&word, data + offset, size - offset);
        Accumulate(acc, word, keys[lane] + tail_key, lane);
    }
    u64 hash = static_cast<u64>(size) * prime;
    for (size_t i = 0; i < 4; ++i) {
        hash ^= Avalanche(acc[i] + keys[i]);
        hash = ((hash << 27) | (hash >> 37)) * prime + key_step;
    }
    return Avalanche(hash);
}

} // namespace detail

inline u64 PageHashScalar(const u8* data, size_t size) {
    using namespace detail;
    std::array<u64, 4> acc = acc_init;
    const size_t stripes = size / 32;
    for (size_t s = 0; s < stripes; ++s) {
        const u64 stripe_key = s * key_step;
        for (size_t lane = 0; lane < 4; ++lane) {
            Accumulate(acc, Load64(data + s * 32 + lane * 8), keys[lane] + stripe_key, lane);
        }
    }
    return Finish(acc, data, size);
}

#ifdef ARCHITECTURE_x86_64
inline u64 PageHashSse2(const u8* data, size_t size) {
    using namespace detail;
    std::array<u64, 4> acc = acc_init;
    const size_t stripes = size / 32;
    __m128i acc01 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(acc.data()));
    __m128i acc23 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(acc.data() + 2));
    __m128i key01 = _mm_set_epi64x(static_cast<s64>(keys[1]), static_cast<s64>(keys[0]));
    __m128i key23 = _mm_set_epi64x(static_cast<s64>(keys[3]), static_cast<s64>(keys[2]));
    const __m128i step = _mm_set1_epi64x(static_cast<s64>(key_step));
    const auto* src = reinterpret_cast<const __m128i*>(data);
    for (size_t s = 0; s < stripes; ++s) {
        const __m128i w01 = _mm_loadu_si128(src + 2 * s);
        const __m128i w23 = _mm_loadu_si128(src + 2 * s + 1);
        const __m128i m01 = _mm_xor_si128(w01, key01);
        const __m128i m23 = _mm_xor_si128(w23, key23);
        acc01 = _mm_add_epi64(acc01, _mm_mul_epu32(m01, _mm_srli_epi64(m01, 32)));
        acc23 = _mm_add_epi64(acc23, _mm_mul_epu32(m23, _mm_srli_epi64(m23, 32)));
        acc01 = _mm_add_epi64(acc01, _mm_shuffle_epi32(w01, 0x4E));
        acc23 = _mm_add_epi64(acc23, _mm_shuffle_epi32(w23, 0x4E));
        key01 = _mm_add_epi64(key01, step);
        key23 = _mm_add_epi64(key23, step);
    }
    _mm_storeu_si128(reinterpret_cast<__m128i*>(acc.data()), acc01);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(acc.data() + 2), acc23);
    return Finish(acc, data, size);
}

NX2_PAGE_HASH_AVX2 inline u64 PageHashAvx2(const u8* data, size_t size) {
    using namespace detail;
    std::array<u64, 4> acc;
    const size_t stripes = size / 32;
    // Two stripes per step into two sums (a + b at the end equals one running sum).
    __m256i sum_a = _mm256_set_epi64x(static_cast<s64>(acc_init[3]), static_cast<s64>(acc_init[2]),
                                      static_cast<s64>(acc_init[1]), static_cast<s64>(acc_init[0]));
    __m256i sum_b = _mm256_setzero_si256();
    const __m256i step = _mm256_set1_epi64x(static_cast<s64>(key_step));
    const __m256i step2 = _mm256_add_epi64(step, step);
    __m256i key_a = _mm256_set_epi64x(static_cast<s64>(keys[3]), static_cast<s64>(keys[2]),
                                      static_cast<s64>(keys[1]), static_cast<s64>(keys[0]));
    __m256i key_b = _mm256_add_epi64(key_a, step);
    const auto* src = reinterpret_cast<const __m256i*>(data);
    size_t s = 0;
    for (; s + 2 <= stripes; s += 2) {
        const __m256i wa = _mm256_loadu_si256(src + s);
        const __m256i wb = _mm256_loadu_si256(src + s + 1);
        const __m256i ma = _mm256_xor_si256(wa, key_a);
        const __m256i mb = _mm256_xor_si256(wb, key_b);
        sum_a = _mm256_add_epi64(sum_a, _mm256_mul_epu32(ma, _mm256_srli_epi64(ma, 32)));
        sum_b = _mm256_add_epi64(sum_b, _mm256_mul_epu32(mb, _mm256_srli_epi64(mb, 32)));
        sum_a = _mm256_add_epi64(sum_a, _mm256_shuffle_epi32(wa, 0x4E));
        sum_b = _mm256_add_epi64(sum_b, _mm256_shuffle_epi32(wb, 0x4E));
        key_a = _mm256_add_epi64(key_a, step2);
        key_b = _mm256_add_epi64(key_b, step2);
    }
    if (s < stripes) {
        const __m256i wa = _mm256_loadu_si256(src + s);
        const __m256i ma = _mm256_xor_si256(wa, key_a);
        sum_a = _mm256_add_epi64(sum_a, _mm256_mul_epu32(ma, _mm256_srli_epi64(ma, 32)));
        sum_a = _mm256_add_epi64(sum_a, _mm256_shuffle_epi32(wa, 0x4E));
    }
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(acc.data()), _mm256_add_epi64(sum_a, sum_b));
    _mm256_zeroupper();
    return Finish(acc, data, size);
}
#endif

/// AVX2 when the host has it, else SSE2 (x86-64) or the portable path.
inline u64 PageHash(const u8* data, size_t size) {
#ifdef ARCHITECTURE_x86_64
    static const bool avx2 = Common::g_cpu_caps.avx2;
    return avx2 ? PageHashAvx2(data, size) : PageHashSse2(data, size);
#else
    return PageHashScalar(data, size);
#endif
}

} // namespace VideoCommon::Nx2PageHash
