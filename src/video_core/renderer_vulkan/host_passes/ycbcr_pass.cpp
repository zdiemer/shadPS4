// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/host_passes/ycbcr_pass.h"

#include "video_core/host_shaders/ycbcr_convert_comp.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan::HostPasses {

void YcbcrPass::Create(vk::Device device_, VmaAllocator allocator, u32 num_images) {
    device = device_;
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {
            .binding = i,
            .descriptorType = i < 2 ? vk::DescriptorType::eCombinedImageSampler
                                    : vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        };
    }
    descriptor_layout =
        Check<"create YCbCr descriptor layout">(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptor,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
    pipeline_layout = Check<"create YCbCr pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*descriptor_layout,
    }));
    const auto shader = CompileSPV(YCBCR_CONVERT_COMP, device);
    const vk::ComputePipelineCreateInfo pipeline_info{
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = shader,
            .pName = "main",
        },
        .layout = *pipeline_layout,
    };
    pipeline =
        Check<"create YCbCr pipeline">(device.createComputePipelineUnique({}, pipeline_info));
    device.destroyShaderModule(shader);
    sampler = Check<"create YCbCr sampler">(device.createSamplerUnique({
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
    }));
    outputs.resize(num_images);
    for (auto& output : outputs) {
        output.image = VideoCore::UniqueImage(device, allocator);
    }
}

bool YcbcrPass::NeedsResize(vk::Extent2D size) const {
    return current_size != vk::Extent2D{} && current_size != size;
}

vk::ImageView YcbcrPass::Render(vk::CommandBuffer cmdbuf, vk::ImageView luma, vk::ImageView chroma,
                                vk::Extent2D size) {
    current_image = (current_image + 1) % outputs.size();
    if (current_size != size) {
        current_size = size;
        for (auto& output : outputs) {
            output.storage_view.reset();
            output.sampled_view.reset();
            output.image.Destroy();
            output.image.Create({
                .flags = vk::ImageCreateFlagBits::eMutableFormat,
                .imageType = vk::ImageType::e2D,
                .format = vk::Format::eR8G8B8A8Unorm,
                .extent = {size.width, size.height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                         vk::ImageUsageFlagBits::eTransferSrc,
            });
            vk::ImageViewCreateInfo view_info{
                .image = output.image,
                .viewType = vk::ImageViewType::e2D,
                .format = vk::Format::eR8G8B8A8Unorm,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
            };
            output.storage_view =
                Check<"create YCbCr storage view">(device.createImageViewUnique(view_info));
            const vk::ImageViewUsageCreateInfo usage{.usage = vk::ImageUsageFlagBits::eSampled};
            view_info.pNext = &usage;
            view_info.format = vk::Format::eR8G8B8A8Srgb;
            output.sampled_view =
                Check<"create YCbCr sampled view">(device.createImageViewUnique(view_info));
        }
    }
    auto& output = outputs[current_image];
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = output.image,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    const std::array images{
        vk::DescriptorImageInfo{*sampler, luma, vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{*sampler, chroma, vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{{}, *output.storage_view, vk::ImageLayout::eGeneral},
    };
    std::array<vk::WriteDescriptorSet, 3> writes{};
    for (u32 i = 0; i < writes.size(); ++i) {
        writes[i] = {
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = i < 2 ? vk::DescriptorType::eCombinedImageSampler
                                    : vk::DescriptorType::eStorageImage,
            .pImageInfo = &images[i],
        };
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.dispatch((size.width + 7) / 8, (size.height + 7) / 8, 1);
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    barrier.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
    barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
    barrier.oldLayout = vk::ImageLayout::eGeneral;
    barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    return *output.sampled_view;
}

vk::Image YcbcrPass::GetImage() const {
    return outputs[current_image].image;
}

} // namespace Vulkan::HostPasses
