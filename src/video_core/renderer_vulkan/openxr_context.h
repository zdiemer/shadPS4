// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include "common/types.h"
#include "input/vr_state.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class OpenXRContext {
public:
    OpenXRContext();
    ~OpenXRContext();

    OpenXRContext(const OpenXRContext&) = delete;
    OpenXRContext& operator=(const OpenXRContext&) = delete;

    bool IsAvailable() const;
    bool IsSessionRunning() const;
    std::span<const std::string> GetInstanceExtensions() const;
    std::span<const std::string> GetDeviceExtensions() const;
    VkPhysicalDevice GetGraphicsDevice(VkInstance instance) const;
    bool CreateSession(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                       u32 queue_family_index);
    using StereoRenderer =
        std::function<bool(const std::array<vk::Image, 2>&, const std::array<vk::Extent2D, 2>&)>;
    bool RenderStereo(const std::array<Input::Vr::Pose, 2>& poses,
                      const std::array<Input::Vr::FieldOfView, 2>& fovs,
                      const StereoRenderer& render);
    void ClearStereo();
    void Update();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
