// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/openxr_context.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <ranges>
#include <string_view>
#include <vector>

#include "common/debug.h"
#include "common/logging/log.h"
#include "input/vr_state.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#ifdef ENABLE_OPENXR
#define XR_USE_GRAPHICS_API_VULKAN
#ifdef _WIN32
#define XR_USE_PLATFORM_WIN32
#include <unknwn.h>
#include <windows.h>
#else
#define XR_USE_TIMESPEC
#include <time.h>
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "video_core/renderer_vulkan/openxr_input.h"
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

Input::Vr::Pose ConvertPose(const XrPosef& pose) {
    return {
        .position = {pose.position.x, pose.position.y, pose.position.z},
        .orientation = {pose.orientation.x, pose.orientation.y, pose.orientation.z,
                        pose.orientation.w},
    };
}

} // namespace

struct OpenXRContext::Impl {
    XrInstance instance{XR_NULL_HANDLE};
    XrSystemId system{XR_NULL_SYSTEM_ID};
    XrSession session{XR_NULL_HANDLE};
    XrSpace local_space{XR_NULL_HANDLE};
    XrSpace view_space{XR_NULL_HANDLE};
    XrSessionState session_state{XR_SESSION_STATE_UNKNOWN};
    bool user_presence_supported{};
    bool user_present{true};
    std::atomic<bool> session_running{};
    struct EyeSwapchain {
        XrSwapchain handle{XR_NULL_HANDLE};
        vk::Extent2D size{};
        std::vector<XrSwapchainImageVulkanKHR> images;
    };
    std::array<EyeSwapchain, 2> swapchains{};
    std::array<XrCompositionLayerProjectionView, 2> projection_views{};
    std::mutex stereo_mutex;
    bool stereo_ready{};
    std::mutex frame_callback_mutex;
    std::function<void(bool)> frame_callback;
    std::unique_ptr<OpenXRInput> input;

    bool CreateSwapchains();
    PFN_xrGetVulkanGraphicsDeviceKHR get_graphics_device{};
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
#ifdef _WIN32
    PFN_xrConvertWin32PerformanceCounterToTimeKHR convert_time{};
#else
    PFN_xrConvertTimespecTimeToTimeKHR convert_time{};
#endif

    std::optional<Input::Vr::DeviceState> Locate(std::chrono::steady_clock::time_point time);
    Input::Vr::DeviceState Locate(XrTime time, Input::Vr::DeviceState state);

    ~Impl() {
        Input::Vr::SetTrackingProvider({});
        Input::Vr::SetDeviceState({});
        input.reset();
        for (const auto& swapchain : swapchains) {
            if (swapchain.handle != XR_NULL_HANDLE) {
                xrDestroySwapchain(swapchain.handle);
            }
        }
        if (view_space != XR_NULL_HANDLE) {
            xrDestroySpace(view_space);
        }
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
    std::vector<const char*> enabled_extensions{XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    const bool frame_profile = std::ranges::any_of(extensions, [](const auto& extension) {
        return std::strcmp(extension.extensionName, "XR_VALVE_frame_controller_interaction") == 0;
    });
    if (frame_profile) {
        enabled_extensions.push_back("XR_VALVE_frame_controller_interaction");
    }
    const bool user_presence_extension = std::ranges::any_of(extensions, [](const auto& extension) {
        return std::strcmp(extension.extensionName, XR_EXT_USER_PRESENCE_EXTENSION_NAME) == 0;
    });
    if (user_presence_extension) {
        enabled_extensions.push_back(XR_EXT_USER_PRESENCE_EXTENSION_NAME);
    }
#ifdef _WIN32
    const char* time_extension = XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME;
#else
    const char* time_extension = XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME;
#endif
    const bool time_supported =
        std::ranges::any_of(extensions, [time_extension](const auto& extension) {
            return std::strcmp(extension.extensionName, time_extension) == 0;
        });
    if (time_supported) {
        enabled_extensions.push_back(time_extension);
    }
    XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(create_info.applicationInfo.applicationName, "shadPS4",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(create_info.applicationInfo.engineName, "shadPS4", XR_MAX_ENGINE_NAME_SIZE - 1);
    create_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    create_info.enabledExtensionCount = static_cast<u32>(enabled_extensions.size());
    create_info.enabledExtensionNames = enabled_extensions.data();
    const XrResult instance_result = xrCreateInstance(&create_info, &context->instance);
    if (XR_FAILED(instance_result)) {
        LOG_WARNING(Render_Vulkan, "Failed to create OpenXR instance: {}",
                    static_cast<s32>(instance_result));
        return;
    }
    if (time_supported) {
#ifdef _WIN32
        context->convert_time = LoadFunction<PFN_xrConvertWin32PerformanceCounterToTimeKHR>(
            context->instance, "xrConvertWin32PerformanceCounterToTimeKHR");
#else
        context->convert_time = LoadFunction<PFN_xrConvertTimespecTimeToTimeKHR>(
            context->instance, "xrConvertTimespecTimeToTimeKHR");
#endif
    }

    context->input = std::make_unique<OpenXRInput>(context->instance);
    if (!context->input->Initialize(frame_profile)) {
        LOG_WARNING(Render_Vulkan, "Failed to initialize OpenXR controller actions");
        context->input.reset();
    }
    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(xrGetSystem(context->instance, &system_info, &context->system))) {
        LOG_WARNING(Render_Vulkan, "No OpenXR headset is available");
        return;
    }
    if (user_presence_extension) {
        XrSystemUserPresencePropertiesEXT presence{XR_TYPE_SYSTEM_USER_PRESENCE_PROPERTIES_EXT};
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
        properties.next = &presence;
        if (XR_SUCCEEDED(xrGetSystemProperties(context->instance, context->system, &properties))) {
            context->user_presence_supported = presence.supportsUserPresence == XR_TRUE;
        }
    }
    context->user_present = !context->user_presence_supported;
    LOG_INFO(Render_Vulkan, "OpenXR user presence sensing: {}", context->user_presence_supported);

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
    if (XR_FAILED(get_requirements(context->instance, context->system, &requirements))) {
        LOG_WARNING(Render_Vulkan, "Failed to query OpenXR Vulkan requirements");
        return;
    }
    constexpr XrVersion vulkan_version = XR_MAKE_VERSION(
        VK_VERSION_MAJOR(TargetVulkanApiVersion), VK_VERSION_MINOR(TargetVulkanApiVersion), 0);
    if (requirements.minApiVersionSupported > vulkan_version) {
        LOG_WARNING(Render_Vulkan, "OpenXR runtime requires an unsupported Vulkan version");
        return;
    }
    if (requirements.maxApiVersionSupported < vulkan_version) {
        LOG_WARNING(Render_Vulkan, "OpenXR runtime has only tested Vulkan {}.{}; using {}.{}",
                    XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
                    XR_VERSION_MINOR(requirements.maxApiVersionSupported),
                    VK_VERSION_MAJOR(TargetVulkanApiVersion),
                    VK_VERSION_MINOR(TargetVulkanApiVersion));
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
    const XrResult result =
        impl->get_graphics_device(impl->instance, impl->system, instance, &physical_device);
    if (XR_FAILED(result)) {
        LOG_WARNING(Render_Vulkan, "OpenXR runtime rejected the Vulkan instance: {}",
                    static_cast<s32>(result));
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
    if (impl->input && !impl->input->Attach(impl->session)) {
        LOG_WARNING(Render_Vulkan, "Failed to attach OpenXR controller actions");
        impl->input.reset();
    }
    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.poseInReferenceSpace.orientation.w = 1.0f;
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (XR_FAILED(xrCreateReferenceSpace(impl->session, &space_info, &impl->local_space))) {
        LOG_WARNING(Render_Vulkan, "Failed to create OpenXR local reference space");
        return false;
    }
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (XR_FAILED(xrCreateReferenceSpace(impl->session, &space_info, &impl->view_space))) {
        LOG_WARNING(Render_Vulkan, "Failed to create OpenXR view reference space");
        return false;
    }
    if (!impl->CreateSwapchains()) {
        for (auto& swapchain : impl->swapchains) {
            if (swapchain.handle != XR_NULL_HANDLE) {
                xrDestroySwapchain(swapchain.handle);
                swapchain.handle = XR_NULL_HANDLE;
            }
            swapchain.images.clear();
        }
        LOG_WARNING(Render_Vulkan, "OpenXR stereo swapchain creation failed");
    }
    Input::Vr::SetDeviceState({.connected = true});
    if (impl->convert_time) {
        Input::Vr::SetTrackingProvider(
            [context = impl.get()](auto time) { return context->Locate(time); });
    } else {
        LOG_WARNING(Render_Vulkan, "OpenXR runtime has no host clock conversion extension");
    }
    LOG_INFO(Render_Vulkan, "OpenXR Vulkan session created");
    return true;
}

bool OpenXRContext::Impl::CreateSwapchains() {
    u32 format_count = 0;
    if (XR_FAILED(xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr))) {
        return false;
    }
    std::vector<int64_t> formats(format_count);
    if (XR_FAILED(
            xrEnumerateSwapchainFormats(session, format_count, &format_count, formats.data())) ||
        std::ranges::find(formats, VK_FORMAT_R8G8B8A8_SRGB) == formats.end()) {
        return false;
    }
    u32 view_count = 0;
    std::array<XrViewConfigurationView, 2> views{};
    for (auto& view : views) {
        view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    }
    if (XR_FAILED(xrEnumerateViewConfigurationViews(instance, system,
                                                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                    views.size(), &view_count, views.data())) ||
        view_count != views.size()) {
        return false;
    }
    for (u32 eye = 0; eye < swapchains.size(); ++eye) {
        auto& swapchain = swapchains[eye];
        swapchain.size = {views[eye].recommendedImageRectWidth,
                          views[eye].recommendedImageRectHeight};
        XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                 XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                                 XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
        create_info.format = VK_FORMAT_R8G8B8A8_SRGB;
        create_info.sampleCount = 1;
        create_info.width = swapchain.size.width;
        create_info.height = swapchain.size.height;
        create_info.faceCount = 1;
        create_info.arraySize = 1;
        create_info.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(session, &create_info, &swapchain.handle))) {
            return false;
        }
        u32 image_count = 0;
        if (XR_FAILED(xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr))) {
            return false;
        }
        swapchain.images.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        if (XR_FAILED(xrEnumerateSwapchainImages(
                swapchain.handle, image_count, &image_count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data())))) {
            return false;
        }
    }
    LOG_INFO(Render_Vulkan, "OpenXR stereo swapchains: {}x{} and {}x{}", swapchains[0].size.width,
             swapchains[0].size.height, swapchains[1].size.width, swapchains[1].size.height);
    return true;
}

bool OpenXRContext::RenderStereo(const std::array<Input::Vr::Pose, 2>& poses,
                                 const std::array<Input::Vr::FieldOfView, 2>& fovs,
                                 const StereoRenderer& render) {
    RENDERER_TRACE;
    if (!impl) {
        return false;
    }
    std::scoped_lock lock{impl->stereo_mutex};
    if (!impl->session_running || impl->swapchains[0].images.empty() ||
        impl->swapchains[1].images.empty()) {
        return false;
    }
    std::array<vk::Image, 2> images{};
    std::array<vk::Extent2D, 2> sizes{};
    u32 acquired = 0;
    auto release = [&] {
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        std::scoped_lock queue_lock{Scheduler::submit_mutex};
        bool success = true;
        for (u32 eye = 0; eye < acquired; ++eye) {
            success &=
                XR_SUCCEEDED(xrReleaseSwapchainImage(impl->swapchains[eye].handle, &release_info));
        }
        return success;
    };
    for (u32 eye = 0; eye < images.size(); ++eye) {
        auto& swapchain = impl->swapchains[eye];
        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        u32 index = 0;
        XrResult result;
        {
            std::scoped_lock queue_lock{Scheduler::submit_mutex};
            result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &index);
        }
        if (XR_FAILED(result)) {
            impl->stereo_ready = false;
            release();
            LOG_WARNING(Render_Vulkan, "OpenXR eye acquire failed: {}", static_cast<s32>(result));
            return false;
        }
        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait_info.timeout = XR_INFINITE_DURATION;
        {
            ZoneScopedN("OpenXR swapchain wait");
            result = xrWaitSwapchainImage(swapchain.handle, &wait_info);
        }
        if (XR_FAILED(result)) {
            impl->stereo_ready = false;
            release();
            LOG_WARNING(Render_Vulkan, "OpenXR eye wait failed: {}", static_cast<s32>(result));
            return false;
        }
        ++acquired;
        images[eye] = swapchain.images[index].image;
        sizes[eye] = swapchain.size;
    }
    const bool rendered = render(images, sizes);
    if (!release() || !rendered) {
        impl->stereo_ready = false;
        return false;
    }
    for (u32 eye = 0; eye < poses.size(); ++eye) {
        auto& view = impl->projection_views[eye];
        view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        const auto& pose = poses[eye];
        view.pose.position = {pose.position[0], pose.position[1], pose.position[2]};
        view.pose.orientation = {pose.orientation[0], pose.orientation[1], pose.orientation[2],
                                 pose.orientation[3]};
        view.fov = {fovs[eye].left, fovs[eye].right, fovs[eye].up, fovs[eye].down};
        view.subImage.swapchain = impl->swapchains[eye].handle;
        view.subImage.imageRect.extent = {static_cast<int32_t>(sizes[eye].width),
                                          static_cast<int32_t>(sizes[eye].height)};
    }
    impl->stereo_ready = true;
    return true;
}

void OpenXRContext::ClearStereo() {
    if (impl) {
        std::scoped_lock lock{impl->stereo_mutex};
        impl->stereo_ready = false;
    }
}

std::optional<Input::Vr::DeviceState> OpenXRContext::Impl::Locate(
    std::chrono::steady_clock::time_point time) {
    const auto snapshot = Input::Vr::GetDeviceState();
    if (!snapshot.connected || !snapshot.session_running || !convert_time) {
        return std::nullopt;
    }
    XrTime xr_time{};
#ifdef _WIN32
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const auto now = std::chrono::steady_clock::now();
    if (XR_FAILED(convert_time(instance, &counter, &xr_time))) {
        return std::nullopt;
    }
#else
    timespec counter{};
    if (clock_gettime(CLOCK_MONOTONIC, &counter) != 0) {
        return std::nullopt;
    }
    const auto now = std::chrono::steady_clock::now();
    if (XR_FAILED(convert_time(instance, &counter, &xr_time))) {
        return std::nullopt;
    }
#endif
    const auto offset = std::chrono::duration_cast<std::chrono::nanoseconds>(time - now).count();
    if (xr_time <= 0 || offset <= -xr_time ||
        offset > std::numeric_limits<XrTime>::max() - xr_time) {
        return std::nullopt;
    }
    xr_time += offset;
    return Locate(xr_time, {
                               .connected = snapshot.connected,
                               .session_running = snapshot.session_running,
                               .mounted = snapshot.mounted,
                               .controllers = snapshot.controllers,
                               .sample_time = time,
                           });
}

Input::Vr::DeviceState OpenXRContext::Impl::Locate(XrTime time, Input::Vr::DeviceState state) {
    if (input) {
        input->Locate(local_space, time, state);
    }
    XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    location.next = &velocity;
    if (XR_SUCCEEDED(xrLocateSpace(view_space, local_space, time, &location))) {
        state.orientation_valid =
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
        state.position_valid = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
        state.orientation_tracked =
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;
        state.position_tracked =
            (location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;
        if (state.orientation_valid) {
            state.head_pose.orientation = ConvertPose(location.pose).orientation;
        }
        if (state.position_valid) {
            state.head_pose.position = ConvertPose(location.pose).position;
        }
        state.linear_velocity_valid =
            (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
        state.angular_velocity_valid =
            (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0;
        if (state.linear_velocity_valid) {
            state.linear_velocity = {velocity.linearVelocity.x, velocity.linearVelocity.y,
                                     velocity.linearVelocity.z};
        }
        if (state.angular_velocity_valid) {
            state.angular_velocity = {velocity.angularVelocity.x, velocity.angularVelocity.y,
                                      velocity.angularVelocity.z};
        }
    }
    XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate_info.displayTime = time;
    locate_info.space = local_space;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> views{XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
    u32 view_count = 0;
    if (XR_SUCCEEDED(xrLocateViews(session, &locate_info, &view_state,
                                   static_cast<u32>(views.size()), &view_count, views.data())) &&
        view_count == views.size() &&
        (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0 &&
        (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0) {
        state.eyes_valid = true;
        for (size_t eye = 0; eye < views.size(); ++eye) {
            state.eye_poses[eye] = ConvertPose(views[eye].pose);
            const auto& fov = views[eye].fov;
            state.field_of_view[eye] = {fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown};
        }
    }
    return state;
}

void OpenXRContext::SetFrameCallback(std::function<void(bool)> callback) {
    if (impl) {
        std::scoped_lock lock{impl->frame_callback_mutex};
        impl->frame_callback = std::move(callback);
    }
}

void OpenXRContext::Update() {
    RENDERER_TRACE;
    if (!impl || impl->session == XR_NULL_HANDLE || impl->local_space == XR_NULL_HANDLE ||
        impl->view_space == XR_NULL_HANDLE) {
        return;
    }
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(impl->instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (changed.session != impl->session) {
                event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
                continue;
            }
            std::scoped_lock stereo_lock{impl->stereo_mutex};
            impl->session_state = changed.state;
            if (changed.state == XR_SESSION_STATE_READY) {
                impl->user_present = !impl->user_presence_supported;
                XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
                begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                impl->session_running = XR_SUCCEEDED(xrBeginSession(impl->session, &begin_info));
            } else if (changed.state == XR_SESSION_STATE_STOPPING && impl->session_running) {
                xrEndSession(impl->session);
                impl->session_running = false;
                impl->stereo_ready = false;
            } else if (changed.state == XR_SESSION_STATE_EXITING ||
                       changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                impl->session_running = false;
                impl->stereo_ready = false;
            }
            LOG_INFO(Render_Vulkan, "OpenXR session state: {}", static_cast<s32>(changed.state));
            Input::Vr::SetDeviceState({
                .connected = changed.state != XR_SESSION_STATE_EXITING &&
                             changed.state != XR_SESSION_STATE_LOSS_PENDING,
                .session_running = impl->session_running,
                .mounted = impl->session_running && impl->user_present,
            });
        } else if (event.type == XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT &&
                   impl->user_presence_supported) {
            const auto& changed =
                *reinterpret_cast<const XrEventDataUserPresenceChangedEXT*>(&event);
            if (changed.session == impl->session) {
                impl->user_present = changed.isUserPresent == XR_TRUE;
                auto state = Input::Vr::GetDeviceState();
                state.mounted = impl->session_running && impl->user_present;
                Input::Vr::SetDeviceState(state);
                LOG_INFO(Render_Vulkan, "OpenXR user present: {}", impl->user_present);
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            std::scoped_lock stereo_lock{impl->stereo_mutex};
            impl->session_running = false;
            impl->stereo_ready = false;
            Input::Vr::SetDeviceState({});
        }
        event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }

    if (!impl->session_running) {
        return;
    }

    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    XrResult wait_result;
    {
        ZoneScopedN("OpenXR frame wait");
        wait_result = xrWaitFrame(impl->session, &wait_info, &frame_state);
    }
    if (XR_FAILED(wait_result)) {
        return;
    }
    std::function<void(bool)> frame_callback;
    {
        std::scoped_lock lock{impl->frame_callback_mutex};
        frame_callback = impl->frame_callback;
    }
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    {
        std::scoped_lock queue_lock{Scheduler::submit_mutex};
        if (XR_FAILED(xrBeginFrame(impl->session, &begin_info))) {
            return;
        }
    }
    if (frame_callback) {
        frame_callback(true);
    }
    Input::Vr::DeviceState state{
        .connected = true,
        .session_running = true,
        .mounted = impl->user_present,
        .sample_time = std::chrono::steady_clock::now(),
    };
    if (const auto current = impl->Locate(state.sample_time)) {
        state = *current;
    }
    if (impl->input) {
        impl->input->Sync(impl->session_state == XR_SESSION_STATE_FOCUSED, state);
        impl->input->Locate(impl->local_space, frame_state.predictedDisplayTime, state);
    }
    Input::Vr::SetDeviceState(state);
    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = frame_state.predictedDisplayTime;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    XrResult result;
    {
        ZoneScopedN("OpenXR frame end");
        std::scoped_lock stereo_lock{impl->stereo_mutex};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.space = impl->local_space;
        projection.viewCount = impl->projection_views.size();
        projection.views = impl->projection_views.data();
        const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        if (frame_state.shouldRender && impl->stereo_ready) {
            end_info.layerCount = 1;
            end_info.layers = &layer;
        }
        std::scoped_lock queue_lock{Scheduler::submit_mutex};
        result = xrEndFrame(impl->session, &end_info);
    }
    if (XR_FAILED(result)) {
        LOG_WARNING(Render_Vulkan, "OpenXR end frame failed: {}", static_cast<s32>(result));
    } else if (frame_callback) {
        frame_callback(false);
    }
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

bool OpenXRContext::RenderStereo(const std::array<Input::Vr::Pose, 2>&,
                                 const std::array<Input::Vr::FieldOfView, 2>&,
                                 const StereoRenderer&) {
    return false;
}

void OpenXRContext::ClearStereo() {}

void OpenXRContext::SetFrameCallback(std::function<void(bool)>) {}

void OpenXRContext::Update() {}

#endif

} // namespace Vulkan
