// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>

namespace Input::Vr {

struct Pose {
    std::array<float, 3> position{};
    std::array<float, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};
};

struct FieldOfView {
    float left{};
    float right{};
    float up{};
    float down{};
};

struct DeviceState {
    bool connected{};
    bool session_running{};
    bool mounted{};
    bool orientation_valid{};
    bool position_valid{};
    bool orientation_tracked{};
    bool position_tracked{};
    Pose head_pose{};
    std::array<Pose, 2> eye_poses{};
    std::array<FieldOfView, 2> field_of_view{};
    std::array<float, 3> linear_velocity{};
    std::array<float, 3> angular_velocity{};
    bool linear_velocity_valid{};
    bool angular_velocity_valid{};
    std::chrono::steady_clock::time_point sample_time{};
};

DeviceState GetDeviceState();
void SetDeviceState(const DeviceState& state);

} // namespace Input::Vr
