// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/hmd/hmd.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/videoout_error.h"
#include "imgui/renderer/imgui_core.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Libraries::VideoOut {

constexpr static bool Is32BppPixelFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::A8R8G8B8Srgb:
    case PixelFormat::A8B8G8R8Srgb:
    case PixelFormat::A2R10G10B10:
    case PixelFormat::A2R10G10B10Srgb:
    case PixelFormat::A2R10G10B10Bt2020Pq:
        return true;
    default:
        return false;
    }
}

constexpr u32 PixelFormatBpp(PixelFormat pixel_format) {
    switch (pixel_format) {
    case PixelFormat::A16R16G16B16Float:
        return 8;
    default:
        return 4;
    }
}

VideoOutDriver::VideoOutDriver(u32 width, u32 height) {
    for (auto* port : {&main_port, &social_port}) {
        port->resolution.full_width = width;
        port->resolution.full_height = height;
        port->resolution.pane_width = width;
        port->resolution.pane_height = height;
        port->vblank_period = 1000000000 / EmulatorSettings.GetVblankFrequency();
    }
    present_thread = std::jthread([&](std::stop_token token) { PresentThread(token); });
    vblank_threads[0] =
        std::jthread([&](std::stop_token token) { VblankThread(&main_port, token); });
    vblank_threads[1] =
        std::jthread([&](std::stop_token token) { VblankThread(&social_port, token); });
}

VideoOutDriver::~VideoOutDriver() {
    present_thread.request_stop();
    for (auto& thread : vblank_threads) {
        thread.request_stop();
    }
    present_thread.join();
    for (auto& thread : vblank_threads) {
        thread.join();
    }
}

int VideoOutDriver::Open(s32 bus_type, const ServiceThreadParams* params) {
    const s32 handle = bus_type == SCE_VIDEO_OUT_BUS_TYPE_MAIN ? 1 : 2;
    auto* port = GetPort(handle);
    std::scoped_lock lock{mutex};
    if (port->is_open) {
        return ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY;
    }
    port->is_open = true;
    liverpool->SetVoPort(port, handle - 1);
    return handle;
}

void VideoOutDriver::Close(s32 handle) {
    auto* port = GetPort(handle);
    std::scoped_lock lock{mutex};
    std::scoped_lock port_lock{port->vo_mutex};

    // Mark as closed
    port->is_open = false;
    port->flip_rate = 0;
    port->prev_index = -1;
    last_flip_vblank[handle - 1] = ~u64{};

    // Clear port information
    std::memset(port->buffer_labels.data(), 0, sizeof(port->buffer_labels));
    std::memset(port->groups.data(), 0, sizeof(port->groups));
    port->vblank_status = {};
    port->vblank_period = 1000000000 / EmulatorSettings.GetVblankFrequency();
    port->resolution.refresh_rate = SCE_VIDEO_OUT_REFRESH_RATE_59_94HZ;
    port->flip_status = FlipStatus{};

    // Re-initialize buffers
    std::memset(port->buffer_slots.data(), 0, sizeof(port->buffer_slots));
    for (auto& buffer : port->buffer_slots) {
        buffer.group_index = -1;
    }

    // Clear events
    for (auto event : port->flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                                Kernel::OrbisKernelEvent::Filter::VideoOut, port);
        }
    }
    port->flip_events.clear();
    for (auto event : port->vblank_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                                Kernel::OrbisKernelEvent::Filter::VideoOut, port);
        }
    }
    port->vblank_events.clear();
}

VideoOutPort* VideoOutDriver::GetPort(int handle) {
    switch (handle) {
    case 1:
        return &main_port;
    case 2:
        return &social_port;
    default:
        return nullptr;
    }
}

int VideoOutDriver::RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses,
                                    s32 bufferNum, const BufferAttribute* attribute) {
    const bool is_ycbcr = attribute->pixel_format == PixelFormat::Ycbcr420Bt709;
    if (!Is32BppPixelFormat(attribute->pixel_format) && !is_ycbcr) {
        LOG_ERROR(Lib_VideoOut,
                  "Unsupported pixel format = {:#x}, width = {}, height = {}, pitch = {}, "
                  "tiling = {}",
                  static_cast<u32>(attribute->pixel_format), attribute->width, attribute->height,
                  attribute->pitch_in_pixel, static_cast<s32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PIXEL_FORMAT;
    }
    if (is_ycbcr && attribute->tiling_mode != TilingMode::Linear) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }
    if (is_ycbcr && (attribute->width == 0 || attribute->height == 0 ||
                     (attribute->width | attribute->height) % 2 != 0)) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_RESOLUTION;
    }
    if (is_ycbcr && attribute->pitch_in_pixel % 64 != 0) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    const s32 group_index = port->FindFreeGroup();
    if (group_index >= MaxDisplayBufferGroups) {
        return ORBIS_VIDEO_OUT_ERROR_NO_EMPTY_SLOT;
    }

    if (startIndex + bufferNum > MaxDisplayBuffers || startIndex > MaxDisplayBuffers ||
        bufferNum > MaxDisplayBuffers) {
        LOG_ERROR(Lib_VideoOut,
                  "Attempted to register too many buffers startIndex = {}, bufferNum = {}",
                  startIndex, bufferNum);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    const s32 end_index = startIndex + bufferNum;
    if (bufferNum > 0 &&
        std::any_of(port->buffer_slots.begin() + startIndex, port->buffer_slots.begin() + end_index,
                    [](auto& buffer) { return buffer.group_index != -1; })) {
        return ORBIS_VIDEO_OUT_ERROR_SLOT_OCCUPIED;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "startIndex = {}, bufferNum = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             startIndex, bufferNum, GetPixelFormatString(attribute->pixel_format),
             attribute->aspect_ratio, static_cast<u32>(attribute->tiling_mode), attribute->width,
             attribute->height, attribute->pitch_in_pixel, attribute->option);

    auto& group = port->groups[group_index];
    std::memcpy(&group.attrib, attribute, sizeof(BufferAttribute));
    group.is_occupied = true;

    for (u32 i = 0; i < bufferNum; i++) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(addresses[i]);
        port->buffer_slots[startIndex + i] = VideoOutBuffer{
            .group_index = group_index,
            .address_left = address,
            .address_right = 0,
        };

        // Reset flip label also when registering buffer
        port->buffer_labels[startIndex + i] = 0;
        port->SignalVoLabel();

        presenter->RegisterVideoOutSurface(group, address);
        LOG_INFO(Lib_VideoOut, "buffers[{}] = {:#x}", i + startIndex, address);
    }

    return group_index;
}

int VideoOutDriver::UnregisterBuffers(VideoOutPort* port, s32 attributeIndex) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    auto& group = port->groups[attributeIndex];
    group.is_occupied = false;

    for (auto& buffer : port->buffer_slots) {
        if (buffer.group_index != attributeIndex) {
            continue;
        }
        buffer.group_index = -1;
    }

    return ORBIS_OK;
}

int VideoOutDriver::ChangeBufferAttribute(VideoOutPort* port, s32 attributeIndex,
                                          const BufferAttribute* attribute) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "attributeIndex = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             attributeIndex, GetPixelFormatString(attribute->pixel_format), attribute->aspect_ratio,
             static_cast<u32>(attribute->tiling_mode), attribute->width, attribute->height,
             attribute->pitch_in_pixel, attribute->option);

    std::unique_lock lock{port->port_mutex};
    std::memcpy(&port->groups[attributeIndex].attrib, attribute, sizeof(BufferAttribute));
    return 0;
}

void VideoOutDriver::Flip(const Request& req) {
    // Update HDR status before presenting.
    presenter->SetHDR(req.port->is_hdr);

    // Present the frame.
    presenter->Present(req.frame);

    // Update flip status.
    auto* port = req.port;
    {
        std::unique_lock lock{port->port_mutex};
        auto& flip_status = port->flip_status;
        flip_status.count++;
        flip_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
        flip_status.tsc = Libraries::Kernel::sceKernelReadTsc();
        flip_status.flip_arg = req.flip_arg;
        flip_status.current_buffer = req.index;
        if (req.eop) {
            --flip_status.gc_queue_num;
        }
        --flip_status.flip_pending_num;
    }

    // Trigger flip events for the port.
    for (auto event : port->flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->TriggerEvent(
                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                Kernel::OrbisKernelEvent::Filter::VideoOut,
                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |
                                        (req.flip_arg << 16)),
                port);
        }
    }

    // Reset prev flip label
    if (port->prev_index != -1) {
        port->buffer_labels[port->prev_index] = 0;
        port->SignalVoLabel();
    }
    // save to prev buf index
    port->prev_index = req.index;
}

void VideoOutDriver::DrawBlankFrame() {
    const auto empty_frame = presenter->PrepareBlankFrame(true);
    presenter->Present(empty_frame, false, false);
}

void VideoOutDriver::DrawLastFrame() {
    const auto frame = presenter->PrepareLastFrame();
    if (frame != nullptr) {
        presenter->Present(frame, true);
    }
}

bool VideoOutDriver::SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg,
                                bool is_eop /*= false*/) {
    {
        std::unique_lock lock{port->port_mutex};
        if (index != -1 && port->flip_status.flip_pending_num > 16) {
            LOG_ERROR(Lib_VideoOut, "Flip queue is full");
            return false;
        }

        if (is_eop) {
            ++port->flip_status.gc_queue_num;
        }
        ++port->flip_status.flip_pending_num; // integral GPU and CPU pending flips counter
        port->flip_status.submit_tsc = Libraries::Kernel::sceKernelReadTsc();
    }

    if (!is_eop) {
        // Non EOP flips can arrive from any thread so ask GPU thread to perform them
        liverpool->SendCommand([=, this]() { SubmitFlipInternal(port, index, flip_arg, is_eop); });
    } else {
        SubmitFlipInternal(port, index, flip_arg, is_eop);
    }

    return true;
}

void VideoOutDriver::SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop) {
    Vulkan::Frame* frame;
    if (index == -1) {
        frame = presenter->PrepareBlankFrame(false);
    } else {
        const auto& buffer = port->buffer_slots[index];
        ASSERT_MSG(buffer.group_index >= 0, "Trying to flip an unregistered buffer!");
        const auto& group = port->groups[buffer.group_index];
        frame = presenter->PrepareFrame(group, buffer.address_left);
    }

    std::scoped_lock lock{mutex};
    requests[port == &main_port ? 0 : 1].push({
        .frame = frame,
        .port = port,
        .flip_arg = flip_arg,
        .index = index,
        .eop = is_eop,
    });
}

int VideoOutDriver::SetRefreshRate(VideoOutPort* port, u64 refresh_rate) {
    s64 period;
    switch (refresh_rate) {
    case static_cast<u64>(SCE_VIDEO_OUT_REFRESH_RATE_ANY):
        return ORBIS_OK;
    case SCE_VIDEO_OUT_REFRESH_RATE_23_98HZ:
        period = 1001000000000 / 24000;
        break;
    case SCE_VIDEO_OUT_REFRESH_RATE_50HZ:
        period = 1000000000 / 50;
        break;
    case SCE_VIDEO_OUT_REFRESH_RATE_59_94HZ:
        period = 1001000000000 / 60000;
        break;
    case SCE_VIDEO_OUT_REFRESH_RATE_119_88HZ:
        period = 1001000000000 / 120000;
        break;
    case SCE_VIDEO_OUT_REFRESH_RATE_89_91HZ:
        period = 1001000000000 / 90000;
        break;
    default:
        return ORBIS_VIDEO_OUT_ERROR_UNSUPPORTED_OUTPUT_MODE;
    }
    std::scoped_lock lock{port->vo_mutex};
    port->resolution.refresh_rate = refresh_rate;
    port->vblank_period = period;
    LOG_INFO(Lib_VideoOut, "Output refresh rate = {}, vblank period = {} ns", refresh_rate, period);
    return ORBIS_OK;
}

void VideoOutDriver::PresentThread(std::stop_token token) {
    Common::SetCurrentThreadName("shadPS4:PresentThread");
    u64 last_tick{};

    const auto receive_request = [this] -> Request {
        std::scoped_lock lk{mutex};
        for (u32 offset = 0; offset < requests.size(); ++offset) {
            const u32 index = (next_request_port + offset) % requests.size();
            auto& queue = requests[index];
            if (queue.empty()) {
                continue;
            }
            const auto request = queue.front();
            auto* port = request.port;
            std::scoped_lock port_lock{port->vo_mutex};
            const auto count = port->vblank_status.count / (port->flip_rate + 1);
            if (count != last_flip_vblank[index]) {
                queue.pop();
                last_flip_vblank[index] = count;
                next_request_port = (index + 1) % requests.size();
                return request;
            }
        }
        return {};
    };

    while (!token.stop_requested()) {
        {
            std::unique_lock lock{mutex};
            Common::CondvarWait(present_cv, lock, token, [&] { return present_tick != last_tick; });
            last_tick = present_tick;
        }
        if (token.stop_requested()) {
            break;
        }

        if (DebugState.IsGuestThreadsPaused()) {
            DrawLastFrame();
            continue;
        }

        bool flipped = false;
        while (const auto request = receive_request()) {
            Flip(request);
            FRAME_END;
            flipped = true;
        }
        if (!flipped) {
            if (!main_port.is_open && !social_port.is_open) {
                DrawBlankFrame();
            } else if (ImGui::Core::MustKeepDrawing()) {
                DrawLastFrame();
            }
        }
    }
}

void VideoOutDriver::VblankThread(VideoOutPort* port, std::stop_token token) {
    const s32 handle = port == &main_port ? 1 : 2;
    auto period = port->GetVblankPeriod();
    Common::SetCurrentThreadName(port == &main_port ? "shadPS4:MainVblankThread"
                                                    : "shadPS4:SocialVblankThread");
    Common::SetCurrentThreadRealtime(period);
    Common::AccurateTimer timer{period};
    while (!token.stop_requested()) {
        const auto requested_period = port->GetVblankPeriod();
        if (period != requested_period) {
            period = requested_period;
            timer = Common::AccurateTimer{period};
        }
        timer.Start();
        if (!DebugState.IsGuestThreadsPaused()) {
            Hmd::NotifyReprojection(handle, true);
            {
                std::scoped_lock lock{port->vo_mutex};
                auto& vblank_status = port->vblank_status;
                for (auto event : port->vblank_events) {
                    auto equeue = Kernel::GetEqueue(event);
                    if (equeue != nullptr) {
                        equeue->TriggerEvent(
                            static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                            Kernel::OrbisKernelEvent::Filter::VideoOut,
                            reinterpret_cast<void*>(
                                static_cast<u64>(OrbisVideoOutInternalEventId::Vblank) |
                                (vblank_status.count << 16)),
                            port);
                    }
                }
                vblank_status.count++;
                vblank_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
                vblank_status.tsc = Libraries::Kernel::sceKernelReadTsc();
                port->vblank_cv.notify_all();
            }
            Hmd::NotifyReprojection(handle, false);
        }
        {
            std::scoped_lock lock{mutex};
            ++present_tick;
        }
        present_cv.notify_one();
        timer.End();
    }
}

} // namespace Libraries::VideoOut
