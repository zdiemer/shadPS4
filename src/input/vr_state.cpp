// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/vr_state.h"

#include <mutex>

namespace Input::Vr {

namespace {

std::mutex g_mutex;
DeviceState g_state;
std::mutex g_provider_mutex;
TrackingProvider g_provider;

} // namespace

DeviceState GetDeviceState() {
    std::scoped_lock lock{g_mutex};
    return g_state;
}

void SetDeviceState(const DeviceState& state) {
    std::scoped_lock lock{g_mutex};
    g_state = state;
}

std::optional<DeviceState> LocateDevice(std::chrono::steady_clock::time_point time) {
    std::scoped_lock lock{g_provider_mutex};
    return g_provider ? g_provider(time) : std::nullopt;
}

void SetTrackingProvider(TrackingProvider provider) {
    std::scoped_lock lock{g_provider_mutex};
    g_provider = std::move(provider);
}

std::array<float, 4> RelativeOrientation(const std::array<float, 4>& orientation,
                                         const std::array<float, 4>& origin) {
    const auto& a = origin;
    const auto& b = orientation;
    return {
        a[3] * b[0] - a[0] * b[3] - a[1] * b[2] + a[2] * b[1],
        a[3] * b[1] + a[0] * b[2] - a[1] * b[3] - a[2] * b[0],
        a[3] * b[2] - a[0] * b[1] + a[1] * b[0] - a[2] * b[3],
        a[3] * b[3] + a[0] * b[0] + a[1] * b[1] + a[2] * b[2],
    };
}

std::array<float, 3> RotateToLocal(const std::array<float, 4>& q, const std::array<float, 3>& v) {
    const std::array<float, 3> t{
        -2.0f * (q[1] * v[2] - q[2] * v[1]),
        -2.0f * (q[2] * v[0] - q[0] * v[2]),
        -2.0f * (q[0] * v[1] - q[1] * v[0]),
    };
    return {
        v[0] + q[3] * t[0] - q[1] * t[2] + q[2] * t[1],
        v[1] + q[3] * t[1] - q[2] * t[0] + q[0] * t[2],
        v[2] + q[3] * t[2] - q[0] * t[1] + q[1] * t[0],
    };
}

} // namespace Input::Vr
