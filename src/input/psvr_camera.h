// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

#include <SDL3/SDL_camera.h>
#include "common/types.h"

namespace Input {

struct StereoCameraFrame {
    int width{};
    int height{};
    std::array<std::vector<u8>, 2> images;
    u64 timestamp_ns{};
    u64 sequence{};
    std::chrono::steady_clock::time_point sample_time;
};

class PsvrCamera {
public:
    ~PsvrCamera();
    static bool IsDevice(SDL_CameraID device);
    bool Open(SDL_CameraID device, int width = 1280, int framerate = 60);
    void Close();
    std::shared_ptr<const StereoCameraFrame> ReadFrame();

private:
    std::mutex mutex;
    SDL_Camera* camera{};
    int width{};
    int height{};
    u64 sequence{};
    std::shared_ptr<const StereoCameraFrame> latest_frame;
};

} // namespace Input
