// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/hmd/hmd.h"
#include "core/libraries/hmd/hmd_error.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/libs.h"
#include "core/libraries/videoout/buffer.h"
#include "core/libraries/videoout/video_out.h"
#include "core/memory.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;

namespace Libraries::Hmd {

namespace {
std::mutex g_reprojection_mutex;
bool g_initialized{};
bool g_buffers_set{};
s32 g_display_handle{};

struct UserEvent {
    Kernel::OrbisKernelEqueue queue{};
    s32 id{};
};

std::optional<UserEvent> g_start_event;
std::optional<UserEvent> g_end_event;

void ResetReprojectionState() {
    if (presenter) {
        presenter->StopVr();
    }
    g_initialized = false;
    g_buffers_set = false;
    g_display_handle = 0;
    g_start_event.reset();
    g_end_event.reset();
}

s32 SetUserEvent(std::optional<UserEvent>& event, Kernel::OrbisKernelEqueue queue, s32 id) {
    std::scoped_lock lock{g_reprojection_mutex};
    if (event) {
        return ORBIS_HMD_ERROR_REPROJECTION_RESOURCE_ALREADY_SET;
    }
    auto* equeue = Kernel::GetEqueue(queue);
    if (!equeue || !equeue->EventExists(id, Kernel::OrbisKernelEvent::Filter::User)) {
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    event = UserEvent{queue, id};
    return ORBIS_OK;
}

s32 ClearUserEvent(std::optional<UserEvent>& event) {
    std::scoped_lock lock{g_reprojection_mutex};
    if (!g_initialized) {
        return ORBIS_HMD_ERROR_REPROJECTION_NOT_INITIALIZED;
    }
    if (!event) {
        return ORBIS_HMD_ERROR_REPROJECTION_RESOURCE_NOT_SET;
    }
    event.reset();
    return ORBIS_OK;
}

s32 ReadLayer(const OrbisHmdReprojectionRenderParam* param, VideoCore::VrLayer& layer) {
    const auto label_address = reinterpret_cast<VAddr>(param->label);
    if (param->label && (label_address % alignof(u64) != 0 ||
                         !Core::Memory::Instance()->IsValidMapping(label_address, sizeof(u64)))) {
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    layer.release_label = param->label;
    const std::array pointers{param->left_image, param->right_image};
    const std::array transforms{param->left_uv, param->right_uv};
    for (u32 eye = 0; eye < pointers.size(); ++eye) {
        const auto address = reinterpret_cast<VAddr>(pointers[eye]);
        if (!Core::Memory::Instance()->IsValidMapping(address, sizeof(AmdGpu::Image))) {
            return ORBIS_HMD_ERROR_PARAMETER_INVALID;
        }
        std::memcpy(&layer.images[eye], pointers[eye], sizeof(AmdGpu::Image));
        const auto& image = layer.images[eye];
        if (!image.Valid() || image.GetBaseType() != AmdGpu::ImageType::Color2D ||
            image.NumSamples() != 1 || AmdGpu::IsBlockCoded(image.GetDataFmt()) ||
            !Core::Memory::Instance()->IsValidGpuMapping(image.Address(), 16)) {
            return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
        }
        std::copy_n(transforms[eye], 4, layer.uv_transform[eye].begin());
    }
    return ORBIS_OK;
}

bool HasFiniteTransforms(const VideoCore::VrLayer& layer) {
    return std::ranges::all_of(layer.uv_transform, [](const auto& transform) {
        return std::ranges::all_of(transform, [](float value) { return std::isfinite(value); });
    });
}

s32 ReadSampler(const void* pointer, AmdGpu::Sampler& sampler) {
    if (pointer == nullptr) {
        return ORBIS_HMD_ERROR_PARAMETER_NULL;
    }
    if (!Core::Memory::Instance()->IsValidMapping(reinterpret_cast<VAddr>(pointer),
                                                  sizeof(sampler))) {
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    std::memcpy(&sampler, pointer, sizeof(sampler));
    if (!sampler.Valid() || sampler.border_color_type == AmdGpu::BorderColor::Custom) {
        return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
    }
    return ORBIS_OK;
}

s32 SubmitReprojection(const OrbisHmdReprojectionRenderParam* param,
                       const OrbisHmdReprojectionPose* pose, u64 frame_number,
                       const OrbisHmdReprojectionRenderParam* overlay, u32 flags) {
    std::scoped_lock lock{g_reprojection_mutex};
    if (!g_initialized) {
        return ORBIS_HMD_ERROR_REPROJECTION_NOT_INITIALIZED;
    }
    if (!g_buffers_set) {
        return ORBIS_HMD_ERROR_REPROJECTION_NO_DISPLAY_BUFFER;
    }
    if (param == nullptr || pose == nullptr) {
        return ORBIS_HMD_ERROR_PARAMETER_NULL;
    }
    auto* memory = Core::Memory::Instance();
    if (flags != 0 || !presenter ||
        !memory->IsValidMapping(reinterpret_cast<VAddr>(param), sizeof(*param)) ||
        !memory->IsValidMapping(reinterpret_cast<VAddr>(pose), sizeof(*pose)) ||
        (overlay && !memory->IsValidMapping(reinterpret_cast<VAddr>(overlay), sizeof(*overlay)))) {
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    VideoCore::VrFrame frame{};
    frame.frame_number = frame_number;
    std::copy_n(pose->position, 3, frame.head_pose.position.begin());
    std::copy_n(pose->orientation, 4, frame.head_pose.orientation.begin());
    if (!std::ranges::all_of(frame.head_pose.position,
                             [](float value) { return std::isfinite(value); })) {
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    float norm = 0;
    for (float value : frame.head_pose.orientation) {
        norm += value * value;
    }
    const bool empty_pose =
        std::ranges::all_of(frame.head_pose.orientation, [](float value) { return value == 0.0f; });
    if (!std::isfinite(norm) || (!empty_pose && norm < 0.000001f)) {
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    if (!empty_pose) {
        for (float& value : frame.head_pose.orientation) {
            value /= std::sqrt(norm);
        }
    }
    if (const s32 result = ReadLayer(param, frame.scene); result != ORBIS_OK) {
        return result;
    }
    if (!HasFiniteTransforms(frame.scene)) {
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    if (overlay != nullptr) {
        if (const s32 result = ReadLayer(overlay, frame.overlay.emplace()); result != ORBIS_OK) {
            return result;
        }
        if (!HasFiniteTransforms(*frame.overlay)) {
            return ORBIS_HMD_ERROR_PARAMETER_INVALID;
        }
    }
    presenter->SubmitVrFrame(frame);
    return ORBIS_OK;
}

} // namespace

void ResetReprojection() {
    std::scoped_lock lock{g_reprojection_mutex};
    ResetReprojectionState();
}

void NotifyReprojection(s32 handle, bool start, bool primary_output) {
    std::scoped_lock lock{g_reprojection_mutex};
    if (!g_initialized || (g_buffers_set ? handle != g_display_handle : !primary_output)) {
        return;
    }
    const auto& event = start ? g_start_event : g_end_event;
    if (event) {
        if (auto* equeue = Kernel::GetEqueue(event->queue)) {
            equeue->TriggerEvent(event->id, Kernel::OrbisKernelEvent::Filter::User, nullptr);
        }
    }
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartMultilayer(const OrbisHmdReprojectionLayer* layers,
                                                   u32 count,
                                                   const OrbisHmdReprojectionMultilayerParam* param,
                                                   const OrbisHmdReprojectionPose* pose,
                                                   u64 frame_number, u32 flags) {
    std::scoped_lock lock{g_reprojection_mutex};
    if (!g_initialized) {
        return ORBIS_HMD_ERROR_REPROJECTION_NOT_INITIALIZED;
    }
    if (!g_buffers_set) {
        return ORBIS_HMD_ERROR_REPROJECTION_NO_DISPLAY_BUFFER;
    }
    if (layers == nullptr || param == nullptr || pose == nullptr) {
        return ORBIS_HMD_ERROR_PARAMETER_NULL;
    }
    if (count == 0 || flags != 0 || !presenter) {
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    if (count > 2) {
        return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
    }
    auto* memory = Core::Memory::Instance();
    if (!memory->IsValidMapping(reinterpret_cast<VAddr>(layers), sizeof(*layers) * count) ||
        !memory->IsValidMapping(reinterpret_cast<VAddr>(param), sizeof(*param)) ||
        !memory->IsValidMapping(reinterpret_cast<VAddr>(pose), sizeof(*pose))) {
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    VideoCore::VrFrame frame{};
    frame.frame_number = frame_number;
    std::copy_n(pose->position, 3, frame.head_pose.position.begin());
    std::copy_n(pose->orientation, 4, frame.head_pose.orientation.begin());
    if (!std::ranges::all_of(frame.head_pose.position,
                             [](float value) { return std::isfinite(value); })) {
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    float norm = 0;
    for (float value : frame.head_pose.orientation) {
        norm += value * value;
    }
    const bool empty_pose =
        std::ranges::all_of(frame.head_pose.orientation, [](float value) { return value == 0.0f; });
    if (!std::isfinite(norm) || (!empty_pose && norm < 0.000001f)) {
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    if (!empty_pose) {
        for (float& value : frame.head_pose.orientation) {
            value /= std::sqrt(norm);
        }
    }
    if (count == 2) {
        frame.overlay.emplace();
    }
    const std::array destinations{&frame.scene, frame.overlay ? &*frame.overlay : nullptr};
    for (u32 i = 0; i < count; ++i) {
        const auto& source = layers[i];
        if (source.left_depth || source.right_depth || source.projection ||
            source.flags != (i == 0 ? 0u : 2u)) {
            return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
        }
        AmdGpu::Sampler sampler{};
        if (const s32 result = ReadSampler(source.sampler, sampler); result != ORBIS_OK) {
            return result;
        }
        OrbisHmdReprojectionRenderParam stereo_param{};
        stereo_param.left_image = source.left_image;
        stereo_param.right_image = source.right_image;
        stereo_param.label = param->label;
        std::copy_n(source.left_uv, 4, stereo_param.left_uv);
        std::copy_n(source.right_uv, 4, stereo_param.right_uv);
        auto& destination = *destinations[i];
        if (const s32 result = ReadLayer(&stereo_param, destination); result != ORBIS_OK) {
            return result;
        }
        if (!HasFiniteTransforms(destination)) {
            return ORBIS_HMD_ERROR_PARAMETER_INVALID;
        }
        destination.sampler = sampler;
    }
    presenter->SubmitVrFrame(frame);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionAddDisplayBuffer() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionClearUserEventEnd() {
    return ClearUserEvent(g_end_event);
}

s32 PS4_SYSV_ABI sceHmdReprojectionClearUserEventStart() {
    return ClearUserEvent(g_start_event);
}

s32 PS4_SYSV_ABI sceHmdReprojectionDebugGetLastInfo() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionDebugGetLastInfoMultilayer() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionFinalize() {
    std::scoped_lock lock{g_reprojection_mutex};
    if (!g_initialized) {
        return ORBIS_HMD_ERROR_REPROJECTION_NOT_INITIALIZED;
    }
    ResetReprojectionState();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionFinalizeCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionInitialize(const OrbisHmdReprojectionInitParam* param, u32 mode,
                                              u32 flags) {
    std::scoped_lock lock{g_reprojection_mutex};
    if (g_initialized) {
        return ORBIS_HMD_ERROR_REPROJECTION_ALREADY_INITIALIZED;
    }
    if (param == nullptr) {
        return ORBIS_HMD_ERROR_PARAMETER_NULL;
    }
    if (!Core::Memory::Instance()->IsValidMapping(reinterpret_cast<VAddr>(param), sizeof(*param)) ||
        mode > 2 || flags != 0) {
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    g_initialized = true;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionInitializeCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryGarlicBuffAlign() {
    return 0x100;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryGarlicBuffSize() {
    return 0x100000;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryOnionBuffAlign() {
    return 0x100;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryOnionBuffSize() {
    return 0x810;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetCallback() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetDisplayBuffers(s32 handle, s32 start, s32 count, u32 flags) {
    std::scoped_lock lock{g_reprojection_mutex};
    if (!g_initialized) {
        return ORBIS_HMD_ERROR_REPROJECTION_NOT_INITIALIZED;
    }
    if (handle <= 0 || start < 0 || start >= VideoOut::MaxDisplayBuffers || count <= 0 ||
        count > VideoOut::MaxDisplayBuffers - start || flags != 0) {
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    uintptr_t labels{};
    if (VideoOut::sceVideoOutGetBufferLabelAddress(handle, &labels) < 0) {
        return ORBIS_HMD_ERROR_HANDLE_INVALID;
    }
    g_buffers_set = true;
    g_display_handle = handle;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetOutputMinColor() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetUserEventEnd(s64 queue, s32 id) {
    return SetUserEvent(g_end_event, queue, id);
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetUserEventStart(s64 queue, s32 id) {
    return SetUserEvent(g_start_event, queue, id);
}

s32 PS4_SYSV_ABI sceHmdReprojectionStart(const OrbisHmdReprojectionRenderParam* param,
                                         const OrbisHmdReprojectionPose* pose, u64 frame_number,
                                         u32 flags) {
    return SubmitReprojection(param, pose, frame_number, nullptr, flags);
}

s32 PS4_SYSV_ABI sceHmdReprojectionStart2dVr(const OrbisHmdReprojectionRenderParam2dVr* param,
                                             u64 frame_number, u32 flags) {
    std::scoped_lock lock{g_reprojection_mutex};
    if (!g_initialized) {
        return ORBIS_HMD_ERROR_REPROJECTION_NOT_INITIALIZED;
    }
    if (!g_buffers_set) {
        return ORBIS_HMD_ERROR_REPROJECTION_NO_DISPLAY_BUFFER;
    }
    if (param == nullptr) {
        return ORBIS_HMD_ERROR_PARAMETER_NULL;
    }
    if (flags != 0 || !presenter ||
        !Core::Memory::Instance()->IsValidMapping(reinterpret_cast<VAddr>(param), sizeof(*param))) {
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    AmdGpu::Sampler sampler{};
    if (const s32 result = ReadSampler(param->sampler, sampler); result != ORBIS_OK) {
        return result;
    }
    OrbisHmdReprojectionRenderParam stereo_param{};
    stereo_param.left_image = param->image;
    stereo_param.right_image = param->image;
    stereo_param.label = param->label;
    std::copy_n(param->uv, 4, stereo_param.left_uv);
    std::copy_n(param->uv, 4, stereo_param.right_uv);
    VideoCore::VrFrame frame{};
    frame.frame_number = frame_number;
    frame.head_locked = true;
    if (const s32 result = ReadLayer(&stereo_param, frame.scene); result != ORBIS_OK) {
        return result;
    }
    if (!HasFiniteTransforms(frame.scene)) {
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    frame.scene.sampler = sampler;
    presenter->SubmitVrFrame(frame);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartLiveCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartMultilayer2() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartWideNear() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartWideNearWithOverlay() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartWithOverlay(const OrbisHmdReprojectionRenderParam* param,
                                                    const OrbisHmdReprojectionPose* pose,
                                                    u64 frame_number,
                                                    const OrbisHmdReprojectionRenderParam* overlay,
                                                    u32 flags) {
    if (overlay == nullptr) {
        return ORBIS_HMD_ERROR_PARAMETER_NULL;
    }
    return SubmitReprojection(param, pose, frame_number, overlay, flags);
}

s32 PS4_SYSV_ABI sceHmdReprojectionStop() {
    std::scoped_lock lock{g_reprojection_mutex};
    if (!g_initialized) {
        return ORBIS_HMD_ERROR_REPROJECTION_NOT_INITIALIZED;
    }
    if (presenter) {
        presenter->StopVr();
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStopCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStopLiveCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionUnsetCallback() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionUnsetDisplayBuffers() {
    std::scoped_lock lock{g_reprojection_mutex};
    if (!g_initialized) {
        return ORBIS_HMD_ERROR_REPROJECTION_NOT_INITIALIZED;
    }
    if (presenter) {
        presenter->StopVr();
    }
    g_buffers_set = false;
    g_display_handle = 0;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_A31A0320D80EAD99() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_B9A6FA0735EC7E49() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterReprojection(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("8gH1aLgty5I", "libsceHmdReprojectionMultilayer", 1, "libSceHmd",
                 sceHmdReprojectionStartMultilayer);
    LIB_FUNCTION("NTIbBpSH9ik", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionAddDisplayBuffer);
    LIB_FUNCTION("94+Ggm38KCg", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionClearUserEventEnd);
    LIB_FUNCTION("mdyFbaJj66M", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionClearUserEventStart);
    LIB_FUNCTION("MdV0akauNow", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionDebugGetLastInfo);
    LIB_FUNCTION("ymiwVjPB5+k", "libSceHmd", 1, "libSceHmd",
                 sceHmdReprojectionDebugGetLastInfoMultilayer);
    LIB_FUNCTION("ZrV5YIqD09I", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionFinalize);
    LIB_FUNCTION("utHD2Ab-Ixo", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionFinalizeCapture);
    LIB_FUNCTION("OuygGEWkins", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionInitialize);
    LIB_FUNCTION("BTrQnC6fcAk", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionInitializeCapture);
    LIB_FUNCTION("TkcANcGM0s8", "libSceHmd", 1, "libSceHmd",
                 sceHmdReprojectionQueryGarlicBuffAlign);
    LIB_FUNCTION("z0KtN1vqF2E", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionQueryGarlicBuffSize);
    LIB_FUNCTION("IWybWbR-xvA", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionQueryOnionBuffAlign);
    LIB_FUNCTION("kLUAkN6a1e8", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionQueryOnionBuffSize);
    LIB_FUNCTION("6CRWGc-evO4", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetCallback);
    LIB_FUNCTION("E+dPfjeQLHI", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetDisplayBuffers);
    LIB_FUNCTION("LjdLRysHU6Y", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetOutputMinColor);
    LIB_FUNCTION("knyIhlkpLgE", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetUserEventEnd);
    LIB_FUNCTION("7as0CjXW1B8", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetUserEventStart);
    LIB_FUNCTION("dntZTJ7meIU", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStart);
    LIB_FUNCTION("q3e8+nEguyE", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStart2dVr);
    LIB_FUNCTION("RrvyU1pjb9A", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartCapture);
    LIB_FUNCTION("XZ5QUzb4ae0", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartLiveCapture);
    LIB_FUNCTION("8gH1aLgty5I", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartMultilayer);
    LIB_FUNCTION("gqAG7JYeE7A", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartMultilayer2);
    LIB_FUNCTION("3JyuejcNhC0", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartWideNear);
    LIB_FUNCTION("mKa8scOc4-k", "libSceHmd", 1, "libSceHmd",
                 sceHmdReprojectionStartWideNearWithOverlay);
    LIB_FUNCTION("kcldQ7zLYQQ", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartWithOverlay);
    LIB_FUNCTION("vzMEkwBQciM", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStop);
    LIB_FUNCTION("F7Sndm5teWw", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStopCapture);
    LIB_FUNCTION("PAa6cUL5bR4", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStopLiveCapture);
    LIB_FUNCTION("0wnZViigP9o", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionUnsetCallback);
    LIB_FUNCTION("iGNNpDDjcwo", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionUnsetDisplayBuffers);
    LIB_FUNCTION("oxoDINgOrZk", "libSceHmd", 1, "libSceHmd", Func_A31A0320D80EAD99);
    LIB_FUNCTION("uab6BzXsfkk", "libSceHmd", 1, "libSceHmd", Func_B9A6FA0735EC7E49);
}
} // namespace Libraries::Hmd
