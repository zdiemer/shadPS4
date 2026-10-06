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

} // namespace Input::Vr
