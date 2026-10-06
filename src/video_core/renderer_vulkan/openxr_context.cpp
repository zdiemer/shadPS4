// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/openxr_context.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ranges>
#include <string_view>
#include <vector>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_platform.h"

#ifdef ENABLE_OPENXR
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#endif

namespace Vulkan {

#ifdef ENABLE_OPENXR

namespace {

template <typename Function>
std::vector<std::string> ReadVulkanExtensions(Function function, XrInstance instance,
                                              XrSystemId system) {
    u32 size = 0;
    if (XR_FAILED(function(instance, system, 0, &size, nullptr)) || size == 0) {
        return {};
    }
    std::string names(size, '\0');
    if (XR_FAILED(function(instance, system, size, &size, names.data()))) {
        return {};
    }

    std::vector<std::string> extensions;
    const std::string_view list{names.c_str()};
    size_t start = 0;
    while (start < list.size()) {
        const auto end = list.find(' ', start);
        if (end > start) {
            extensions.emplace_back(list.substr(start, end - start));
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return extensions;
}

template <typename Function>
Function LoadFunction(XrInstance instance, const char* name) {
    PFN_xrVoidFunction function = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &function))) {
        return nullptr;
    }
    return reinterpret_cast<Function>(function);
}

} // namespace

struct OpenXRContext::Impl {
    XrInstance instance{XR_NULL_HANDLE};
    XrSystemId system{XR_NULL_SYSTEM_ID};
    XrSession session{XR_NULL_HANDLE};
    XrSpace local_space{XR_NULL_HANDLE};
    bool session_running{};
    PFN_xrGetVulkanGraphicsDeviceKHR get_graphics_device{};
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;

    ~Impl() {
        if (local_space != XR_NULL_HANDLE) {
            xrDestroySpace(local_space);
        }
        if (session != XR_NULL_HANDLE) {
            xrDestroySession(session);
        }
        if (instance != XR_NULL_HANDLE) {
            xrDestroyInstance(instance);
        }
    }
};

OpenXRContext::OpenXRContext() {
    const char* enabled = std::getenv("SHADPS4_OPENXR");
    if (enabled == nullptr || std::strcmp(enabled, "1") != 0) {
        return;
    }

    u32 extension_count = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr))) {
        LOG_WARNING(Render_Vulkan, "Failed to enumerate OpenXR extensions");
        return;
    }
    std::vector<XrExtensionProperties> extensions(extension_count);
    for (auto& extension : extensions) {
        extension.type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, extension_count, &extension_count,
                                                         extensions.data())) ||
        std::ranges::none_of(extensions, [](const auto& extension) {
            return std::strcmp(extension.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0;
        })) {
        LOG_WARNING(Render_Vulkan, "OpenXR runtime does not support Vulkan integration");
        return;
    }

    auto context = std::make_unique<Impl>();
    const char* required_extension = XR_KHR_VULKAN_ENABLE_EXTENSION_NAME;
    XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(create_info.applicationInfo.applicationName, "shadPS4",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(create_info.applicationInfo.engineName, "shadPS4", XR_MAX_ENGINE_NAME_SIZE - 1);
    create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    create_info.enabledExtensionCount = 1;
    create_info.enabledExtensionNames = &required_extension;
    if (XR_FAILED(xrCreateInstance(&create_info, &context->instance))) {
        LOG_WARNING(Render_Vulkan, "Failed to create OpenXR instance");
        return;
    }

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(xrGetSystem(context->instance, &system_info, &context->system))) {
        LOG_WARNING(Render_Vulkan, "No OpenXR headset is available");
        return;
    }

    const auto get_instance_extensions = LoadFunction<PFN_xrGetVulkanInstanceExtensionsKHR>(
        context->instance, "xrGetVulkanInstanceExtensionsKHR");
    const auto get_device_extensions = LoadFunction<PFN_xrGetVulkanDeviceExtensionsKHR>(
        context->instance, "xrGetVulkanDeviceExtensionsKHR");
    context->get_graphics_device = LoadFunction<PFN_xrGetVulkanGraphicsDeviceKHR>(
        context->instance, "xrGetVulkanGraphicsDeviceKHR");
    const auto get_requirements = LoadFunction<PFN_xrGetVulkanGraphicsRequirementsKHR>(
        context->instance, "xrGetVulkanGraphicsRequirementsKHR");
    if (!get_instance_extensions || !get_device_extensions || !context->get_graphics_device ||
        !get_requirements) {
        LOG_WARNING(Render_Vulkan, "OpenXR Vulkan functions are unavailable");
        return;
    }

    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (XR_FAILED(get_requirements(context->instance, context->system, &requirements)) ||
        XR_VERSION_MAJOR(requirements.minApiVersionSupported) >
            VK_VERSION_MAJOR(TargetVulkanApiVersion) ||
        (XR_VERSION_MAJOR(requirements.minApiVersionSupported) ==
             VK_VERSION_MAJOR(TargetVulkanApiVersion) &&
         XR_VERSION_MINOR(requirements.minApiVersionSupported) >
             VK_VERSION_MINOR(TargetVulkanApiVersion)) ||
        XR_VERSION_MAJOR(requirements.maxApiVersionSupported) <
            VK_VERSION_MAJOR(TargetVulkanApiVersion) ||
        (XR_VERSION_MAJOR(requirements.maxApiVersionSupported) ==
             VK_VERSION_MAJOR(TargetVulkanApiVersion) &&
         XR_VERSION_MINOR(requirements.maxApiVersionSupported) <
             VK_VERSION_MINOR(TargetVulkanApiVersion))) {
        LOG_WARNING(Render_Vulkan, "OpenXR runtime requires an unsupported Vulkan version");
        return;
    }

    context->instance_extensions =
        ReadVulkanExtensions(get_instance_extensions, context->instance, context->system);
    context->device_extensions =
        ReadVulkanExtensions(get_device_extensions, context->instance, context->system);
    if (context->instance_extensions.empty() || context->device_extensions.empty()) {
        LOG_WARNING(Render_Vulkan, "OpenXR runtime did not report Vulkan extensions");
        return;
    }
    impl = std::move(context);
    LOG_INFO(Render_Vulkan, "OpenXR headset detected");
}

OpenXRContext::~OpenXRContext() = default;

bool OpenXRContext::IsAvailable() const {
    return impl != nullptr;
}

bool OpenXRContext::IsSessionRunning() const {
    return impl != nullptr && impl->session_running;
}

std::span<const std::string> OpenXRContext::GetInstanceExtensions() const {
    return impl ? std::span<const std::string>{impl->instance_extensions}
                : std::span<const std::string>{};
}

std::span<const std::string> OpenXRContext::GetDeviceExtensions() const {
    return impl ? std::span<const std::string>{impl->device_extensions}
                : std::span<const std::string>{};
}

VkPhysicalDevice OpenXRContext::GetGraphicsDevice(VkInstance instance) const {
    if (!impl) {
        return VK_NULL_HANDLE;
    }
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    if (XR_FAILED(
            impl->get_graphics_device(impl->instance, impl->system, instance, &physical_device))) {
        LOG_WARNING(Render_Vulkan, "OpenXR runtime rejected the Vulkan instance");
        return VK_NULL_HANDLE;
    }
    return physical_device;
}

bool OpenXRContext::CreateSession(VkInstance instance, VkPhysicalDevice physical_device,
                                  VkDevice device, u32 queue_family_index) {
    if (!impl || impl->session != XR_NULL_HANDLE ||
        GetGraphicsDevice(instance) != physical_device) {
        return false;
    }
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = instance;
    binding.physicalDevice = physical_device;
    binding.device = device;
    binding.queueFamilyIndex = queue_family_index;
    binding.queueIndex = 0;

    XrSessionCreateInfo create_info{XR_TYPE_SESSION_CREATE_INFO};
    create_info.next = &binding;
    create_info.systemId = impl->system;
    const XrResult result = xrCreateSession(impl->instance, &create_info, &impl->session);
    if (XR_FAILED(result)) {
        LOG_WARNING(Render_Vulkan, "Failed to create OpenXR session: {}", static_cast<s32>(result));
        return false;
    }
    LOG_INFO(Render_Vulkan, "OpenXR Vulkan session created");
    return true;
}

void OpenXRContext::Update() {
    if (!impl || impl->session == XR_NULL_HANDLE) {
        return;
    }
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(impl->instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (changed.session == impl->session && changed.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
                begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                impl->session_running = XR_SUCCEEDED(xrBeginSession(impl->session, &begin_info));
            } else if (changed.session == impl->session &&
                       changed.state == XR_SESSION_STATE_STOPPING && impl->session_running) {
                xrEndSession(impl->session);
                impl->session_running = false;
            } else if (changed.session == impl->session &&
                       changed.state == XR_SESSION_STATE_EXITING) {
                impl->session_running = false;
            }
        }
        event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }

    if (!impl->session_running) {
        return;
    }

    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(impl->session, &wait_info, &frame_state))) {
        return;
    }
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(impl->session, &begin_info))) {
        return;
    }
    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = frame_state.predictedDisplayTime;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    xrEndFrame(impl->session, &end_info);
}

#else

struct OpenXRContext::Impl {};

OpenXRContext::OpenXRContext() = default;
OpenXRContext::~OpenXRContext() = default;

bool OpenXRContext::IsAvailable() const {
    return false;
}

bool OpenXRContext::IsSessionRunning() const {
    return false;
}

std::span<const std::string> OpenXRContext::GetInstanceExtensions() const {
    return {};
}

std::span<const std::string> OpenXRContext::GetDeviceExtensions() const {
    return {};
}

VkPhysicalDevice OpenXRContext::GetGraphicsDevice(VkInstance) const {
    return VK_NULL_HANDLE;
}

bool OpenXRContext::CreateSession(VkInstance, VkPhysicalDevice, VkDevice, u32) {
    return false;
}

void OpenXRContext::Update() {}

#endif

} // namespace Vulkan
