// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::HostPasses {

class VrCopyPass {
public:
    void Create(vk::Device device);
    void Render(vk::CommandBuffer cmdbuf, vk::ImageView source, vk::ImageView target,
                vk::Extent2D size, const std::array<float, 4>& bounds, bool overlay = false);

private:
    vk::UniquePipeline pipeline;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniqueDescriptorSetLayout descriptor_layout;
    vk::UniqueSampler sampler;
};

} // namespace Vulkan::HostPasses
