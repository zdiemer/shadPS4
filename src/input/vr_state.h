// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

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

enum class ControllerButton : std::uint32_t {
    Cross = 1 << 0,
    Circle = 1 << 1,
    Square = 1 << 2,
    Triangle = 1 << 3,
    Menu = 1 << 4,
    View = 1 << 5,
    Up = 1 << 6,
    Down = 1 << 7,
    Left = 1 << 8,
    Right = 1 << 9,
    Shoulder = 1 << 10,
    Stick = 1 << 11,
    Select = 1 << 12,
    FacePad = 1 << 13,
};

struct ControllerState {
    bool active{};
    std::uint32_t buttons{};
    std::uint32_t available_buttons{};
    std::array<float, 2> stick{};
    float trigger{};
    float squeeze{};
    Pose grip_pose{};
    Pose aim_pose{};
    bool position_valid{};
    bool orientation_valid{};
    bool position_tracked{};
    bool orientation_tracked{};
    bool aim_valid{};
    std::array<float, 3> linear_velocity{};
    std::array<float, 3> angular_velocity{};
    std::array<float, 3> linear_acceleration{};
    bool linear_velocity_valid{};
    bool angular_velocity_valid{};
    bool linear_acceleration_valid{};
};

enum class ControllerMode {
    Both,
    Pad,
    Move,
};

enum class PadMotionSource {
    Auto,
    Gamepad,
    VrController,
};

struct DeviceState {
    ControllerMode controller_mode{ControllerMode::Both};
    PadMotionSource pad_motion_source{PadMotionSource::Auto};
    bool seated_pad{};
    bool connected{};
    bool session_running{};
    bool mounted{};
    bool orientation_valid{};
    bool position_valid{};
    bool orientation_tracked{};
    bool position_tracked{};
    bool eyes_valid{};
    Pose head_pose{};
    std::array<Pose, 2> eye_poses{};
    std::array<FieldOfView, 2> field_of_view{};
    std::array<ControllerState, 2> controllers{};
    std::array<float, 3> linear_velocity{};
    std::array<float, 3> angular_velocity{};
    bool linear_velocity_valid{};
    bool angular_velocity_valid{};
    std::chrono::steady_clock::time_point sample_time{};
};

struct ControllerSample {
    ControllerState state{};
    std::chrono::steady_clock::time_point time{};
    std::uint64_t sequence{};
};

DeviceState GetDeviceState();
void SetDeviceState(const DeviceState& state);
std::optional<std::array<float, 3>> GetSeatedPadPosition();
bool RecenterSeatedPad();
bool SetControllerVibration(std::size_t hand, std::uint8_t intensity);
std::array<std::uint8_t, 2> GetControllerVibration();
std::vector<ControllerSample> GetControllerHistory(std::size_t hand);
using TrackingProvider =
    std::function<std::optional<DeviceState>(std::chrono::steady_clock::time_point)>;
std::optional<DeviceState> LocateDevice(std::chrono::steady_clock::time_point time);
void SetTrackingProvider(TrackingProvider provider);
std::array<float, 4> RelativeOrientation(const std::array<float, 4>& orientation,
                                         const std::array<float, 4>& origin);
std::array<float, 3> RotateToLocal(const std::array<float, 4>& orientation,
                                   const std::array<float, 3>& vector);

} // namespace Input::Vr
