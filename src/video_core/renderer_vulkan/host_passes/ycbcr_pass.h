// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "video_core/texture_cache/image.h"

namespace Vulkan::HostPasses {

class YcbcrPass {
public:
    void Create(vk::Device device, VmaAllocator allocator, u32 num_images);
    bool NeedsResize(vk::Extent2D size) const;
    vk::ImageView Render(vk::CommandBuffer cmdbuf, vk::ImageView luma, vk::ImageView chroma,
                         vk::Extent2D size);
    vk::Image GetImage() const;

private:
    struct Output {
        VideoCore::UniqueImage image;
        vk::UniqueImageView storage_view;
        vk::UniqueImageView sampled_view;
    };

    vk::Device device{};
    vk::UniqueDescriptorSetLayout descriptor_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline pipeline;
    vk::UniqueSampler sampler;
    std::vector<Output> outputs;
    vk::Extent2D current_size{};
    u32 current_image{};
};

} // namespace Vulkan::HostPasses
