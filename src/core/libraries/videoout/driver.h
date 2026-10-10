// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/debug.h"
#include "common/polyfill_thread.h"
#include "core/libraries/videoout/video_out.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

namespace Vulkan {
struct Frame;
}

namespace Libraries::VideoOut {

struct VideoOutPort {
    SceVideoOutResolutionStatus resolution;
    std::array<VideoOutBuffer, MaxDisplayBuffers> buffer_slots;
    std::array<u64, MaxDisplayBuffers> buffer_labels; // should be contiguous in memory
    static_assert(sizeof(buffer_labels[0]) == 8u);
    std::array<BufferAttributeGroup, MaxDisplayBufferGroups> groups;
    FlipStatus flip_status;
    SceVideoOutVblankStatus vblank_status;
    std::vector<Kernel::OrbisKernelEqueue> flip_events;
    std::vector<Kernel::OrbisKernelEqueue> vblank_events;
    std::mutex vo_mutex;
    std::mutex port_mutex;
    std::condition_variable vo_cv;
    std::condition_variable vblank_cv;
    int flip_rate = 0;
    int prev_index = -1;
    bool is_open = false;
    bool is_hdr = false;
    u64 generation{};
    std::atomic<s64> vblank_period{};

    std::chrono::nanoseconds GetVblankPeriod() const {
        return std::chrono::nanoseconds{vblank_period.load()};
    }

    s32 FindFreeGroup() const {
        s32 index = 0;
        while (index < groups.size() && groups[index].is_occupied) {
            index++;
        }
        return index;
    }

    bool IsVoLabel(const u64* address) const {
        const u64* start = &buffer_labels[0];
        const u64* end = &buffer_labels[MaxDisplayBuffers - 1];
        return address >= start && address <= end;
    }

    void WaitVoLabel(auto&& pred) {
        std::unique_lock lk{vo_mutex};
        vo_cv.wait(lk, pred);
    }

    void SignalVoLabel() {
        std::scoped_lock lk{vo_mutex};
        vo_cv.notify_one();
    }

    [[nodiscard]] int NumRegisteredBuffers() const {
        return std::count_if(buffer_slots.cbegin(), buffer_slots.cend(),
                             [](auto& buffer) { return buffer.group_index != -1; });
    }
};

struct ServiceThreadParams {
    u32 unknown;
    bool set_priority;
    u32 priority;
    bool set_affinity;
    u64 affinity;
};

class VideoOutDriver {
public:
    VideoOutDriver(u32 width, u32 height);
    ~VideoOutDriver();

    int Open(s32 bus_type, const ServiceThreadParams* params);
    void Close(s32 handle);

    VideoOutPort* GetPort(s32 handle);

    int RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses, s32 bufferNum,
                        const BufferAttribute* attribute);
    int UnregisterBuffers(VideoOutPort* port, s32 attributeIndex);
    int ChangeBufferAttribute(VideoOutPort* port, s32 bufferIndex,
                              const BufferAttribute* attribute);
    int SetRefreshRate(VideoOutPort* port, u64 refresh_rate);

    bool SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop = false);
    bool GetReprojectionTarget(s32 handle, s32 start, s32 count,
                               VideoCore::VrDisplayTarget& target);
    bool SubmitReprojectionFlip(const VideoCore::VrDisplayTarget& target, u64 frame_number);
    void StopReprojection();

private:
    struct Request {
        Vulkan::Frame* frame;
        VideoOutPort* port;
        s64 flip_arg;
        s32 index;
        bool eop;
        u64 generation;
        bool pending = true;

        operator bool() const noexcept {
            return frame != nullptr;
        }
    };

    struct ReprojectionScanout {
        s32 start;
        s32 count;
        u64 frame_number;
        u32 buffer_index{};
    };

    void Flip(const Request& req);
    void DrawBlankFrame(); // Video port out not open
    void DrawLastFrame();  // Used when there is no flip request
    void SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop,
                            u64 generation);
    void PresentThread(std::stop_token token);
    void VblankThread(VideoOutPort* port, std::stop_token token);

    std::mutex mutex;
    std::condition_variable_any present_cv;
    u64 present_tick{};
    VideoOutPort main_port{};
    VideoOutPort social_port{};
    std::jthread present_thread;
    std::array<std::queue<Request>, 2> requests;
    std::array<std::optional<ReprojectionScanout>, 2> reprojection_scanouts;
    std::queue<Request> present_requests;
    std::array<std::jthread, 2> vblank_threads;
};

} // namespace Libraries::VideoOut
