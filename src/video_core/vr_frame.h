// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "input/vr_state.h"
#include "video_core/amdgpu/resource.h"

namespace VideoCore {

struct VrFrame {
    std::array<AmdGpu::Image, 2> images{};
    std::array<std::array<float, 4>, 2> uv_transform{};
    Input::Vr::Pose head_pose{};
    u64 frame_number{};
};

} // namespace VideoCore
