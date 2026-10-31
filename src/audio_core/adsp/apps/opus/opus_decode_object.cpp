// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <mutex>
#include <unordered_map>

#include "audio_core/adsp/apps/opus/opus_decode_object.h"
#include "common/assert.h"

namespace AudioCore::ADSP::OpusDecoder {
namespace {
// opus_decoder_get_size of libopus built without the DRED/OSCE neural features. Games size their
// work buffers from what we report, so it must not depend on how the host libopus was built: with
// those features it is several times larger, and games run out of their audio memory budget.
constexpr u32 MonoDecoderSize = 18468;
constexpr u32 StereoDecoderSize = 27236;

struct LibOpusDecoderDeleter {
    void operator()(LibOpusDecoder* decoder) const {
        opus_decoder_destroy(decoder);
    }
};

// The libopus state of each work buffer is kept on the host, as its real size may exceed the
// work buffer size we report.
std::mutex decoders_mutex;
std::unordered_map<const OpusDecodeObject*, std::unique_ptr<LibOpusDecoder, LibOpusDecoderDeleter>>
    decoders;

bool IsValidChannelCount(u32 channel_count) {
    return channel_count == 1 || channel_count == 2;
}
} // namespace

u32 OpusDecodeObject::GetWorkBufferSize(u32 channel_count) {
    if (!IsValidChannelCount(channel_count)) {
        return 0;
    }
    return static_cast<u32>(sizeof(OpusDecodeObject)) +
           (channel_count == 1 ? MonoDecoderSize : StereoDecoderSize);
}

OpusDecodeObject& OpusDecodeObject::Initialize(u64 buffer, u64 buffer2) {
    auto* new_decoder = reinterpret_cast<OpusDecodeObject*>(buffer);
    auto* comparison = reinterpret_cast<OpusDecodeObject*>(buffer2);

    if (new_decoder->magic == DecodeObjectMagic) {
        if (!new_decoder->initialized ||
            (new_decoder->initialized && new_decoder->self == comparison)) {
            new_decoder->state_valid = true;
        }
    } else {
        new_decoder->initialized = false;
        new_decoder->state_valid = true;
    }
    return *new_decoder;
}

void OpusDecodeObject::ReleaseAll() {
    std::scoped_lock lock{decoders_mutex};
    decoders.clear();
}

LibOpusDecoder* OpusDecodeObject::GetDecoder() const {
    std::scoped_lock lock{decoders_mutex};
    const auto it = decoders.find(this);
    return it != decoders.end() ? it->second.get() : nullptr;
}

s32 OpusDecodeObject::InitializeDecoder(u32 sample_rate, u32 channel_count) {
    if (!state_valid) {
        return OPUS_INVALID_STATE;
    }

    if (initialized && GetDecoder() != nullptr) {
        return OPUS_OK;
    }

    int ret{};
    LibOpusDecoder* decoder = opus_decoder_create(static_cast<opus_int32>(sample_rate),
                                                  static_cast<int>(channel_count), &ret);
    if (ret == OPUS_OK) {
        {
            std::scoped_lock lock{decoders_mutex};
            decoders.insert_or_assign(
                this, std::unique_ptr<LibOpusDecoder, LibOpusDecoderDeleter>{decoder});
        }
        magic = DecodeObjectMagic;
        initialized = true;
        state_valid = true;
        self = this;
        final_range = 0;
    }
    return ret;
}

s32 OpusDecodeObject::Shutdown() {
    if (!state_valid) {
        return OPUS_INVALID_STATE;
    }

    if (initialized) {
        {
            std::scoped_lock lock{decoders_mutex};
            decoders.erase(this);
        }
        magic = 0x0;
        initialized = false;
        state_valid = false;
        self = nullptr;
        final_range = 0;
    }
    return OPUS_OK;
}

s32 OpusDecodeObject::ResetDecoder() {
    LibOpusDecoder* decoder = GetDecoder();
    if (decoder == nullptr) {
        return OPUS_INVALID_STATE;
    }
    return opus_decoder_ctl(decoder, OPUS_RESET_STATE);
}

s32 OpusDecodeObject::Decode(u32& out_sample_count, u64 output_data, u64 output_data_size,
                             u64 input_data, u64 input_data_size) {
    ASSERT(initialized);
    out_sample_count = 0;

    LibOpusDecoder* decoder = GetDecoder();
    if (!state_valid || decoder == nullptr) {
        return OPUS_INVALID_STATE;
    }

    auto ret_code_or_samples = opus_decode(
        decoder, reinterpret_cast<const u8*>(input_data), static_cast<opus_int32>(input_data_size),
        reinterpret_cast<opus_int16*>(output_data), static_cast<opus_int32>(output_data_size), 0);

    if (ret_code_or_samples < OPUS_OK) {
        return ret_code_or_samples;
    }

    out_sample_count = ret_code_or_samples;
    return opus_decoder_ctl(decoder, OPUS_GET_FINAL_RANGE_REQUEST, &final_range);
}

} // namespace AudioCore::ADSP::OpusDecoder
