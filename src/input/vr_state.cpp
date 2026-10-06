// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/vr_state.h"

#include <mutex>

namespace Input::Vr {

namespace {

std::mutex g_mutex;
DeviceState g_state;

} // namespace

DeviceState GetDeviceState() {
    std::scoped_lock lock{g_mutex};
    return g_state;
}

void SetDeviceState(const DeviceState& state) {
    std::scoped_lock lock{g_mutex};
    g_state = state;
}

} // namespace Input::Vr
