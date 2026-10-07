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
u64 g_generation{};

struct UserEvent {
    Kernel::OrbisKernelEqueue queue{};
    s32 id{};
};

std::optional<UserEvent> g_start_event;
std::optional<UserEvent> g_end_event;

s32 SetUserEvent(std::optional<UserEvent>& event, Kernel::OrbisKernelEqueue queue, s32 id) {
    std::scoped_lock lock{g_reprojection_mutex};
    if (!g_initialized) {
        return ORBIS_HMD_ERROR_REPROJECTION_NOT_INITIALIZED;
    }
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

void NotifyUserEvent(u64 generation, bool start) {
    std::scoped_lock lock{g_reprojection_mutex};
    if (!g_initialized || !g_buffers_set || generation != g_generation) {
        return;
    }
    const auto& event = start ? g_start_event : g_end_event;
    if (event) {
        if (auto* equeue = Kernel::GetEqueue(event->queue)) {
            equeue->TriggerEvent(event->id, Kernel::OrbisKernelEvent::Filter::User, nullptr);
        }
    }
}

void SetReprojectionNotifications() {
    if (presenter) {
        presenter->SetVrFrameCallback(
            [generation = g_generation](bool start) { NotifyUserEvent(generation, start); });
    }
}

s32 ReadLayer(const OrbisHmdReprojectionRenderParam* param, VideoCore::VrLayer& layer) {
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
    if (flags != 0 || !presenter) {
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
    if (!std::isfinite(norm) || norm < 0.000001f) {
        return ORBIS_OK;
    }
    for (float& value : frame.head_pose.orientation) {
        value /= std::sqrt(norm);
    }
    if (const s32 result = ReadLayer(param, frame.scene); result != ORBIS_OK) {
        return result;
    }
    if (!HasFiniteTransforms(frame.scene)) {
        return ORBIS_OK;
    }
    if (overlay != nullptr) {
        if (const s32 result = ReadLayer(overlay, frame.overlay.emplace()); result != ORBIS_OK) {
            return result;
        }
        if (!HasFiniteTransforms(*frame.overlay)) {
            return ORBIS_OK;
        }
    }
    SetReprojectionNotifications();
    presenter->SubmitVrFrame(frame);
    return ORBIS_OK;
}

} // namespace

s32 PS4_SYSV_ABI sceHmdReprojectionStartMultilayer() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
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
    if (presenter) {
        presenter->SetVrFrameCallback({});
        presenter->StopVr();
    }
    g_initialized = false;
    g_buffers_set = false;
    ++g_generation;
    g_start_event.reset();
    g_end_event.reset();
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
    if (mode > 2 || flags != 0) {
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    g_initialized = true;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionInitializeCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryGarlicBuffAlign() { return 0x100; }

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
    if (handle <= 0 || start < 0 || start >= 16 || count <= 0 || count > 16 - start || flags != 0) {
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;
    }
    uintptr_t labels{};
    if (VideoOut::sceVideoOutGetBufferLabelAddress(handle, &labels) < 0) {
        return ORBIS_HMD_ERROR_HANDLE_INVALID;
    }
    g_buffers_set = true;
    SetReprojectionNotifications();
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

s32 PS4_SYSV_ABI sceHmdReprojectionStart2dVr() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
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
    ++g_generation;
    if (presenter) {
        presenter->SetVrFrameCallback({});
        presenter->StopVr();
    }
    g_buffers_set = false;
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
