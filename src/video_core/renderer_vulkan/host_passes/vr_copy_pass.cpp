// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/host_passes/vr_copy_pass.h"

#include "common/assert.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/vr_copy_frag.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan::HostPasses {

void VrCopyPass::Create(vk::Device device) {
    constexpr vk::Format surface_format = vk::Format::eR8G8B8A8Srgb;
    const std::array bindings{
        vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
    };

    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci{
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };

    descriptor_layout = Check<"create VR descriptor set layout">(
        device.createDescriptorSetLayoutUnique(desc_layout_ci));

    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(Settings),
    };

    const auto& vs_module = CompileSPV(FS_TRI_VERT, device);
    ASSERT(vs_module);
    SetObjectName(device, vs_module, "fs_tri.vert");

    const auto& fs_module = CompileSPV(VR_COPY_FRAG, device);
    ASSERT(fs_module);
    SetObjectName(device, fs_module, "vr_copy.frag");

    const std::array shaders_ci{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = vs_module,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = fs_module,
            .pName = "main",
        },
    };

    const vk::PipelineLayoutCreateInfo layout_info{
        .setLayoutCount = 1U,
        .pSetLayouts = &*descriptor_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };

    pipeline_layout =
        Check<"create VR pipeline layout">(device.createPipelineLayoutUnique(layout_info));

    const std::array pp_color_formats{
        surface_format,
    };
    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci{
        .colorAttachmentCount = pp_color_formats.size(),
        .pColorAttachmentFormats = pp_color_formats.data(),
    };

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info{
        .vertexBindingDescriptionCount = 0u,
        .vertexAttributeDescriptionCount = 0u,
    };

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };

    const vk::Viewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = 1.0f,
        .height = 1.0f,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    const vk::Rect2D scissor = {
        .offset = {0, 0},
        .extent = {1, 1},
    };

    const vk::PipelineViewportStateCreateInfo viewport_info{
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };

    const vk::PipelineRasterizationStateCreateInfo raster_state{
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eBack,
        .frontFace = vk::FrontFace::eClockwise,
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };

    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };

    const std::array attachments{
        vk::PipelineColorBlendAttachmentState{
            .blendEnable = true,
            .srcColorBlendFactor = vk::BlendFactor::eOne,
            .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
            .colorBlendOp = vk::BlendOp::eAdd,
            .srcAlphaBlendFactor = vk::BlendFactor::eOne,
            .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
            .alphaBlendOp = vk::BlendOp::eAdd,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        },
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending{
        .logicOpEnable = false,
        .logicOp = vk::LogicOp::eCopy,
        .attachmentCount = attachments.size(),
        .pAttachments = attachments.data(),
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    const std::array dynamic_states{
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };

    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = dynamic_states.size(),
        .pDynamicStates = dynamic_states.data(),
    };

    const vk::GraphicsPipelineCreateInfo pipeline_info{
        .pNext = &pipeline_rendering_ci,
        .stageCount = shaders_ci.size(),
        .pStages = shaders_ci.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *pipeline_layout,
    };

    pipeline =
        Check<"create VR copy pipeline">(device.createGraphicsPipelineUnique({}, pipeline_info));

    device.destroyShaderModule(vs_module);
    device.destroyShaderModule(fs_module);

    const vk::SamplerCreateInfo sampler_ci{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
    };
    sampler = Check<"create VR sampler">(device.createSamplerUnique(sampler_ci));
}

void VrCopyPass::Render(vk::CommandBuffer cmdbuf, vk::ImageView source, vk::ImageView target,
                        vk::Extent2D size, const std::array<float, 4>& bounds, bool overlay,
                        vk::Sampler source_sampler) {
    const vk::RenderingAttachmentInfo attachment{
        .imageView = target,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = overlay ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue{.color{std::array{0.0f, 0.0f, 0.0f, 1.0f}}},
    };
    const vk::RenderingInfo rendering{
        .renderArea{.extent = size},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    };
    const vk::DescriptorImageInfo image{
        .sampler = source_sampler ? source_sampler : *sampler,
        .imageView = source,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet write{
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &image,
    };
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
    cmdbuf.setViewport(0, vk::Viewport{.width = static_cast<float>(size.width),
                                       .height = static_cast<float>(size.height),
                                       .maxDepth = 1.0f});
    cmdbuf.setScissor(0, vk::Rect2D{.extent = size});
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0, write);
    const Settings settings{bounds, source_sampler ? 0u : 1u};
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(settings),
                         &settings);
    cmdbuf.beginRendering(rendering);
    cmdbuf.draw(3, 1, 0, 0);
    cmdbuf.endRendering();
}

} // namespace Vulkan::HostPasses
