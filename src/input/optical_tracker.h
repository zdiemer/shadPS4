// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <optional>

#include "input/psvr_camera.h"

namespace Input {

class OpticalTracker {
public:
    bool LoadCalibration(const std::filesystem::path& path);
    std::optional<std::array<float, 3>> Locate(const StereoCameraFrame& frame,
                                               const std::array<u8, 3>& colour);

private:
    bool calibrated{};
    int width{};
    int height{};
    std::array<std::array<double, 4>, 2> intrinsics{};
    std::array<std::array<double, 5>, 2> distortion{};
    std::array<double, 9> rotation{};
    std::array<double, 3> translation{};
    u64 cached_sequence{};
    std::array<u8, 3> cached_colour{};
    std::optional<std::array<float, 3>> cached_position;
};

} // namespace Input
