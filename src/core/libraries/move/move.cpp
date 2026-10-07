// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/libraries/move/move.h"
#include "core/libraries/move/move_error.h"
#include "imgui/renderer/imgui_core.h"
#include "input/controller.h"
#include "input/vr_state.h"

namespace Libraries::Move {

static bool g_library_initialized = false;
static std::mutex g_mutex;
static std::chrono::steady_clock::time_point g_clock_origin{};

struct MoveController {
    s32 user_id{};
    s32 type{};
    s32 index{};
};

static std::unordered_map<s32, MoveController> g_controllers;

static std::optional<size_t> GetControllerIndexLocked(s32 handle) {
    const auto it = g_controllers.find(handle);
    if (it == g_controllers.end() || it->second.type != 0 || it->second.index < 0 ||
        it->second.index >= 2 ||
        Input::GameControllers::GetControllerIndexFromUserID(it->second.user_id) != 0) {
        return std::nullopt;
    }
    return it->second.index;
}

std::optional<size_t> GetControllerIndex(s32 handle) {
    std::scoped_lock lock{g_mutex};
    return GetControllerIndexLocked(handle);
}

static std::optional<size_t> GetConnectedControllerIndexLocked(s32 handle) {
    const auto index = GetControllerIndexLocked(handle);
    const auto state = Input::Vr::GetDeviceState();
    return index && state.session_running && state.mounted && state.controllers[*index].active
               ? index
               : std::nullopt;
}

static OrbisMoveData ConvertSample(const Input::Vr::ControllerSample& sample) {
    OrbisMoveData data{};
    const auto& state = sample.state;
    using Input::Vr::ControllerButton;
    const bool left_faces =
        (state.available_buttons & 0xf) == (std::to_underlying(ControllerButton::Square) |
                                            std::to_underlying(ControllerButton::Triangle));
    const bool shifted = (state.available_buttons & 0xf) != 0xf && state.squeeze > 0.5f;
    const std::array buttons{
        shifted ? OrbisMoveButton::Square : OrbisMoveButton::Cross,
        shifted ? OrbisMoveButton::Triangle : OrbisMoveButton::Circle,
        OrbisMoveButton::Square,
        OrbisMoveButton::Triangle,
        OrbisMoveButton::Start,
        OrbisMoveButton::Select,
        OrbisMoveButton::Triangle,
        OrbisMoveButton::Cross,
        OrbisMoveButton::Square,
        OrbisMoveButton::Circle,
    };
    for (size_t i = 0; i < buttons.size(); ++i) {
        const size_t bit = left_faces && i < 4 ? i ^ 2 : i;
        if (state.buttons & (1u << bit)) {
            if (left_faces && i >= 2 && i < 4) {
                continue;
            }
            data.button_data.button_data |= std::to_underlying(buttons[i]);
        }
    }
    if (state.buttons & std::to_underlying(ControllerButton::FacePad)) {
        const auto& stick = state.stick;
        const auto button =
            std::abs(stick[0]) > std::abs(stick[1])
                ? (stick[0] > 0.0f ? OrbisMoveButton::Circle : OrbisMoveButton::Square)
                : (stick[1] > 0.0f ? OrbisMoveButton::Triangle : OrbisMoveButton::Cross);
        data.button_data.button_data |= std::to_underlying(button);
    }
    if (state.squeeze > 0.5f || (state.buttons & (std::to_underlying(ControllerButton::Shoulder) |
                                                  std::to_underlying(ControllerButton::Select) |
                                                  std::to_underlying(ControllerButton::Stick)))) {
        data.button_data.button_data |= std::to_underlying(OrbisMoveButton::Move);
    }
    if (state.trigger > 0.5f) {
        data.button_data.button_data |= std::to_underlying(OrbisMoveButton::Trigger);
    }
    data.button_data.trigger_data = std::clamp(static_cast<int>(state.trigger * 255.0f), 0, 255);
    if (ImGui::Core::IsGamepadInputCaptured()) {
        data.button_data = {.button_data = std::to_underlying(OrbisMoveButton::Intercepted)};
    }
    if (state.orientation_valid) {
        const auto gravity =
            Input::Vr::RotateToLocal(state.grip_pose.orientation, {0.0f, 1.0f, 0.0f});
        std::copy(gravity.begin(), gravity.end(), data.accelerometer);
        if (state.angular_velocity_valid) {
            const auto gyro =
                Input::Vr::RotateToLocal(state.grip_pose.orientation, state.angular_velocity);
            std::copy(gyro.begin(), gyro.end(), data.gyro);
        }
    }
    data.timestamp = std::max<s64>(
        1, std::chrono::duration_cast<std::chrono::microseconds>(sample.time - g_clock_origin)
               .count());
    data.count = static_cast<s32>((sample.sequence - 1) % std::numeric_limits<s32>::max()) + 1;
    return data;
}

s32 PS4_SYSV_ABI sceMoveInit() {
    std::scoped_lock lock{g_mutex};
    if (g_library_initialized) {
        return ORBIS_MOVE_ERROR_ALREADY_INIT;
    }
    g_clock_origin = std::chrono::steady_clock::now() -
                     std::chrono::microseconds{Kernel::sceKernelGetProcessTime()};
    g_library_initialized = true;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveOpen(Libraries::UserService::OrbisUserServiceUserId user_id, s32 type,
                             s32 index) {
    std::scoped_lock lock{g_mutex};
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    // Even when no controllers are connected, this returns a proper handle.
    // Internal libSceVrTracker logic requires this handle to be different from other devices.
    static s32 handle = 0x30b0000;
    if (handle > std::numeric_limits<s32>::max() - 0x100) {
        return ORBIS_MOVE_ERROR_MAX_CONTROLLERS_EXCEEDED;
    }
    handle += 0x100;
    g_controllers.emplace(handle, MoveController{user_id, type, index});
    return handle;
}

s32 PS4_SYSV_ABI sceMoveGetDeviceInfo(s32 handle, OrbisMoveDeviceInfo* info) {
    std::scoped_lock lock{g_mutex};
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (info == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    if (!g_controllers.contains(handle)) {
        return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    }
    if (!GetConnectedControllerIndexLocked(handle)) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    *info = {.sphere_radius = 22.5f};
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveReadStateLatest(s32 handle, OrbisMoveData* data) {
    std::scoped_lock lock{g_mutex};
    LOG_TRACE(Lib_Move, "(called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (data == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    if (!g_controllers.contains(handle)) {
        return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    }
    const auto index = GetConnectedControllerIndexLocked(handle);
    if (!index) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    const auto samples = Input::Vr::GetControllerHistory(*index);
    if (samples.empty() || !samples.back().state.active) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    *data = ConvertSample(samples.back());
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveReadStateRecent(s32 handle, s64 timestamp, OrbisMoveData* data,
                                        s32* out_count) {
    std::scoped_lock lock{g_mutex};
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (timestamp < 0 || data == nullptr || out_count == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    if (!g_controllers.contains(handle)) {
        return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    }
    *out_count = 0;
    const auto index = GetConnectedControllerIndexLocked(handle);
    if (!index) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    const auto samples = Input::Vr::GetControllerHistory(*index);
    for (const auto& sample : samples) {
        if (!sample.state.active) {
            continue;
        }
        const auto report = ConvertSample(sample);
        if (report.timestamp > timestamp) {
            data[(*out_count)++] = report;
        }
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveGetExtensionPortInfo(s32 handle, void* data) {
    std::scoped_lock lock{g_mutex};
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (data == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveSetVibration(s32 handle, u8 intensity) {
    std::scoped_lock lock{g_mutex};
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveSetLightSphere(s32 handle, u8 red, u8 green, u8 blue) {
    std::scoped_lock lock{g_mutex};
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (!g_controllers.contains(handle)) {
        return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    }
    return GetConnectedControllerIndexLocked(handle) ? ORBIS_OK
                                                     : ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveResetLightSphere(s32 handle) {
    std::scoped_lock lock{g_mutex};
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    // Returns success, even if no controllers are connected.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveClose(s32 handle) {
    std::scoped_lock lock{g_mutex};
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    return g_controllers.erase(handle) ? ORBIS_OK : ORBIS_MOVE_ERROR_INVALID_HANDLE;
}

s32 PS4_SYSV_ABI sceMoveTerm() {
    std::scoped_lock lock{g_mutex};
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    g_library_initialized = false;
    g_controllers.clear();
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("j1ITE-EoJmE", "libSceMove", 1, "libSceMove", sceMoveInit);
    LIB_FUNCTION("HzC60MfjJxU", "libSceMove", 1, "libSceMove", sceMoveOpen);
    LIB_FUNCTION("GWXTyxs4QbE", "libSceMove", 1, "libSceMove", sceMoveGetDeviceInfo);
    LIB_FUNCTION("ttU+JOhShl4", "libSceMove", 1, "libSceMove", sceMoveReadStateLatest);
    LIB_FUNCTION("f2bcpK6kJfg", "libSceMove", 1, "libSceMove", sceMoveReadStateRecent);
    LIB_FUNCTION("y5h7f8H1Jnk", "libSceMove", 1, "libSceMove", sceMoveGetExtensionPortInfo);
    LIB_FUNCTION("IFQwtT2CeY0", "libSceMove", 1, "libSceMove", sceMoveSetVibration);
    LIB_FUNCTION("T8KYHPs1JE8", "libSceMove", 1, "libSceMove", sceMoveSetLightSphere);
    LIB_FUNCTION("zuxWAg3HAac", "libSceMove", 1, "libSceMove", sceMoveResetLightSphere);
    LIB_FUNCTION("XX6wlxpHyeo", "libSceMove", 1, "libSceMove", sceMoveClose);
    LIB_FUNCTION("tsZi60H4ypY", "libSceMove", 1, "libSceMove", sceMoveTerm);
};

} // namespace Libraries::Move
