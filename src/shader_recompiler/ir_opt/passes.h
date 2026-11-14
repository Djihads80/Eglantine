// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <vector>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader {
struct HostTranslateInfo;
}

namespace Shader::Optimization {

void CollectShaderInfoPass(Environment& env, IR::Program& program);
void ConditionalBarrierPass(IR::Program& program);
void ConstantPropagationPass(Environment& env, IR::Program& program);
void DeadCodeEliminationPass(IR::Program& program);
void GeometryCompactionPass(IR::Program& program, const HostTranslateInfo& host_info);
void GlobalMemoryToStorageBufferPass(Environment& env, IR::Program& program,
                                     const HostTranslateInfo& host_info);
/// How many storage buffers a program may bind: MAX_SSBOS, or fewer if the device's per-stage or
/// descriptor set limit is lower
size_t StorageBufferLimit(const HostTranslateInfo& host_info);
void IdentityRemovalPass(IR::Program& program);
void LowerFp64ToFp32(IR::Program& program);
void LowerFp16ToFp32(IR::Program& program);
void LowerInt64ToInt32(IR::Program& program);
void RescalingPass(IR::Program& program);
void SsaRewritePass(IR::Program& program);
void PositionPass(Environment& env, IR::Program& program);
void BlitConstantsPass(Environment& env, IR::Program& program);
void TexturePass(Environment& env, IR::Program& program, const HostTranslateInfo& host_info);
void LimitSampledTextureArrays(IR::Program& program, const HostTranslateInfo& host_info);
/// Reapplies a pipeline's array limit after reconstructing a cached stage's IR.
void LimitSampledTextureArrayElements(IR::Program& program, u32 cap);
/// Shrinks the texture arrays of a pipeline's stages to fit the descriptor set limits, which count
/// every stage; LimitSampledTextureArrays sizes a stage's arrays as if it had them to itself
void LimitPipelineSampledTextureArrays(std::span<IR::Program* const> programs,
                                       const HostTranslateInfo& host_info);
/// Task and mesh programs emulated through compute share one host stage, including its auxiliary
/// storage buffers. Fit their arrays to that stage's sampler, sampled-image and resource limits.
void LimitSharedStageSampledTextureArrays(std::span<IR::Program* const> programs,
                                          const HostTranslateInfo& host_info,
                                          u32 auxiliary_resources);
void LayerPass(IR::Program& program, const HostTranslateInfo& host_info);
void VendorWorkaroundPass(IR::Program& program);
void VerificationPass(const IR::Program& program);
bool HasBrokenPattern(const IR::Program& program);

// Dual Vertex
void VertexATransformPass(IR::Program& program);
void VertexBTransformPass(IR::Program& program);
/// The index in base each descriptor of source has once JoinTextureInfo added it, by kind
struct JoinedDescriptors {
    std::vector<u32> texture_buffers;
    std::vector<u32> image_buffers;
    std::vector<u32> textures;
    std::vector<u32> images;
};
/// Adds the descriptors of source to base, with the handle programs they use; those read the
/// storage buffers of source at the bindings storage_bindings gives them in base (see
/// JoinStorageInfo), or at their own bindings when it gives none
JoinedDescriptors JoinTextureInfo(Info& base, Info& source,
                                  std::span<const u32> storage_bindings = {});
/// Adds the storage buffers of source to base, at most limit in all, returning the binding each
/// has in base
std::vector<u32> JoinStorageInfo(Info& base, Info& source, size_t limit = Info::MAX_SSBOS);
/// Points the texture and image instructions of a program whose descriptors JoinTextureInfo
/// added to another at the indices they have there
void RemapTextureDescriptors(IR::Program& program, const JoinedDescriptors& descriptors);
/// Points the storage buffer instructions of a program whose storage buffers JoinStorageInfo
/// added to another at the bindings they have there
void RemapStorageBindings(IR::Program& program, std::span<const u32> bindings);

} // namespace Shader::Optimization
