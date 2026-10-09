// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>

namespace Input::Vr {

class GamepadMotion {
public:
    void Update(const std::array<float, 3>& acceleration, const std::array<float, 3>& gyro,
                std::uint64_t timestamp);
    void Recenter(const std::array<float, 4>& head_orientation);
    bool HasOrientation() const {
        return initialized;
    }
    const std::array<float, 4>& GetOrientation() const {
        return orientation;
    }
    std::array<float, 3> GetAngularVelocity(const std::array<float, 3>& gyro) const;

private:
    bool initialized{};
    std::uint64_t last_timestamp{};
    std::array<float, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 3> gyro_bias{};
    std::array<float, 3> previous_gravity{};
    float stationary_time{};
};

} // namespace Input::Vr
