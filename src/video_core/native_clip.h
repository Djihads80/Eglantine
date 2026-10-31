// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "video_core/engines/maxwell_3d.h"

namespace VideoCommon {
inline bool RasterizerFrustumClipCompatible(const Tegra::Engines::Maxwell3D::Regs& regs) {
    using Type = Tegra::Engines::Maxwell3D::Regs::ShaderType;
    return !regs.IsShaderConfigEnabled(Type::VertexA) &&
           !regs.IsShaderConfigEnabled(Type::TessellationInit) &&
           !regs.IsShaderConfigEnabled(Type::Tessellation) &&
           !regs.IsShaderConfigEnabled(Type::Geometry) && !regs.transform_feedback_enabled &&
           regs.viewport_clip_control.geometry_clip ==
               Tegra::Engines::Maxwell3D::Regs::ViewportClipControl::GeometryClip::FrustumXYZ;
}
} // namespace VideoCommon
