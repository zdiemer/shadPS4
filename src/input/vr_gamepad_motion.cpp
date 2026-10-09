// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/vr_gamepad_motion.h"

#include <algorithm>
#include <cmath>

namespace Input::Vr {
namespace {

float Length(const std::array<float, 3>& vector) {
    return std::hypot(vector[0], vector[1], vector[2]);
}

std::array<float, 4> Multiply(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] + a[1] * b[3] + a[2] * b[0] - a[0] * b[2],
            a[3] * b[2] + a[2] * b[3] + a[0] * b[1] - a[1] * b[0],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

void Normalize(std::array<float, 4>& q) {
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (auto& value : q) {
        value /= length;
    }
}

float Heading(const std::array<float, 4>& q) {
    return std::atan2(2.0f * (q[0] * q[2] + q[1] * q[3]),
                      1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]));
}

} // namespace

void GamepadMotion::Update(const std::array<float, 3>& acceleration,
                           const std::array<float, 3>& gyro, std::uint64_t timestamp) {
    if (!std::ranges::all_of(acceleration, [](float value) { return std::isfinite(value); }) ||
        !std::ranges::all_of(gyro, [](float value) { return std::isfinite(value); })) {
        return;
    }
    constexpr float Gravity = 9.80665f;
    const float magnitude = Length(acceleration);
    const bool gravity_valid = std::abs(magnitude - Gravity) < Gravity * 0.15f;
    std::array<float, 3> gravity{};
    if (gravity_valid) {
        for (size_t axis = 0; axis < 3; ++axis) {
            gravity[axis] = acceleration[axis] / magnitude;
        }
    }
    if (!initialized) {
        if (!gravity_valid) {
            return;
        }
        orientation = {-gravity[2], 0.0f, gravity[0], 1.0f + gravity[1]};
        if (orientation[3] < 0.00001f) {
            orientation = {1.0f, 0.0f, 0.0f, 0.0f};
        }
        Normalize(orientation);
        initialized = true;
        last_timestamp = timestamp;
        previous_gravity = gravity;
        return;
    }
    if (timestamp <= last_timestamp) {
        return;
    }
    const float interval = static_cast<float>(timestamp - last_timestamp) * 1e-9f;
    last_timestamp = timestamp;
    if (interval > 0.1f) {
        stationary_time = 0.0f;
        return;
    }
    std::array<float, 3> gravity_change{};
    for (size_t axis = 0; axis < 3; ++axis) {
        gravity_change[axis] = gravity[axis] - previous_gravity[axis];
    }
    previous_gravity = gravity;
    if (gravity_valid && std::abs(magnitude - Gravity) < Gravity * 0.05f && Length(gyro) < 0.035f &&
        Length(gravity_change) < 0.02f) {
        stationary_time += interval;
        if (stationary_time >= 0.5f) {
            const float weight = 1.0f - std::exp(-interval / 2.0f);
            for (size_t axis = 0; axis < 3; ++axis) {
                gyro_bias[axis] += weight * (gyro[axis] - gyro_bias[axis]);
            }
        }
    } else {
        stationary_time = 0.0f;
    }
    auto angular_velocity = GetAngularVelocity(gyro);
    if (gravity_valid) {
        const auto& q = orientation;
        const std::array<float, 3> predicted_up{2.0f * (q[0] * q[1] + q[2] * q[3]),
                                                1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]),
                                                2.0f * (q[1] * q[2] - q[0] * q[3])};
        const std::array<float, 3> correction{
            gravity[1] * predicted_up[2] - gravity[2] * predicted_up[1],
            gravity[2] * predicted_up[0] - gravity[0] * predicted_up[2],
            gravity[0] * predicted_up[1] - gravity[1] * predicted_up[0]};
        constexpr float TiltCorrectionRate = 2.0f;
        for (size_t axis = 0; axis < 3; ++axis) {
            angular_velocity[axis] += TiltCorrectionRate * correction[axis];
        }
    }
    const float speed = Length(angular_velocity);
    if (speed > 0.0f) {
        const float half_angle = speed * interval * 0.5f;
        const float scale = std::sin(half_angle) / speed;
        orientation =
            Multiply(orientation, {angular_velocity[0] * scale, angular_velocity[1] * scale,
                                   angular_velocity[2] * scale, std::cos(half_angle)});
        Normalize(orientation);
    }
}

void GamepadMotion::Recenter(const std::array<float, 4>& head_orientation) {
    if (!initialized) {
        return;
    }
    const float half_angle = (Heading(head_orientation) - Heading(orientation)) * 0.5f;
    orientation = Multiply({0.0f, std::sin(half_angle), 0.0f, std::cos(half_angle)}, orientation);
    Normalize(orientation);
}

std::array<float, 3> GamepadMotion::GetAngularVelocity(const std::array<float, 3>& gyro) const {
    return {gyro[0] - gyro_bias[0], gyro[1] - gyro_bias[1], gyro[2] - gyro_bias[2]};
}

} // namespace Input::Vr
