// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/vr_state.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <numbers>

#include "common/logging/log.h"

namespace Input::Vr {

namespace {

std::mutex g_mutex;
DeviceState g_state;
std::atomic<std::int32_t> g_active_user{-1};
std::atomic<std::uint64_t> g_pad_recenter_sequence{};
std::optional<std::array<FieldOfView, 2>> g_render_field_of_view;
std::optional<std::array<float, 3>> g_seated_pad_position;
std::optional<std::array<float, 3>> g_camera_pad_origin;
std::array<float, 3> g_camera_pad_forward{};
std::array<std::uint8_t, 2> g_controller_vibration{};
std::deque<std::array<ControllerSample, 2>> g_controller_history;
std::uint64_t g_controller_sequence{};
std::mutex g_provider_mutex;
TrackingProvider g_provider;

void UpdateSeatedPadPosition(const DeviceState& state) {
    if (!state.session_running || !state.seated_pad) {
        g_seated_pad_position.reset();
        g_camera_pad_origin.reset();
        return;
    }
    if (g_seated_pad_position || !state.mounted || !state.position_valid ||
        !state.orientation_valid) {
        return;
    }
    const auto& q = state.head_pose.orientation;
    const auto forward = RotateToLocal({-q[0], -q[1], -q[2], q[3]}, {0.0f, 0.0f, -1.0f});
    const float length = std::hypot(forward[0], forward[2]);
    if (!std::isfinite(length) || length < 0.001f) {
        return;
    }
    auto position = state.head_pose.position;
    position[0] += 0.5f * forward[0] / length;
    position[1] -= 0.4f;
    position[2] += 0.5f * forward[2] / length;
    for (const auto value : position) {
        if (!std::isfinite(value)) {
            return;
        }
    }
    g_seated_pad_position = position;
    LOG_INFO(Input, "Seated Pad position: {}, {}, {}", position[0], position[1], position[2]);
}

} // namespace

DeviceState GetDeviceState() {
    std::scoped_lock lock{g_mutex};
    return g_state;
}

std::int32_t GetActiveUser() {
    return g_active_user.load();
}

void SetActiveUser(std::int32_t user) {
    std::scoped_lock lock{g_mutex};
    if (g_active_user.exchange(user) != user) {
        g_controller_history.clear();
        g_controller_vibration = {};
    }
}

void ResetTrackingOrigin() {
    std::scoped_lock lock{g_mutex};
    g_seated_pad_position.reset();
    g_camera_pad_origin.reset();
    g_controller_history.clear();
    ++g_pad_recenter_sequence;
}

void SetDeviceState(const DeviceState& state) {
    std::scoped_lock lock{g_mutex};
    if (!state.connected) {
        g_render_field_of_view.reset();
    }
    UpdateSeatedPadPosition(state);
    auto updated_state = state;
    for (size_t hand = 0; hand < updated_state.controllers.size(); ++hand) {
        auto& controller = updated_state.controllers[hand];
        controller.linear_acceleration = {};
        controller.linear_acceleration_valid = false;
        const auto& previous = g_state.controllers[hand];
        if (!state.session_running || !g_state.session_running || !state.mounted ||
            !g_state.mounted || !controller.active || !previous.active ||
            !controller.position_tracked || !previous.position_tracked ||
            !controller.linear_velocity_valid || !previous.linear_velocity_valid ||
            state.sample_time <= g_state.sample_time) {
            continue;
        }
        const float interval =
            std::chrono::duration<float>(state.sample_time - g_state.sample_time).count();
        bool valid = true;
        for (size_t axis = 0; axis < controller.linear_acceleration.size(); ++axis) {
            controller.linear_acceleration[axis] =
                (controller.linear_velocity[axis] - previous.linear_velocity[axis]) / interval;
            valid &= std::isfinite(controller.linear_acceleration[axis]);
        }
        controller.linear_acceleration_valid = valid;
    }
    for (size_t hand = 0; hand < g_controller_vibration.size(); ++hand) {
        if (!state.session_running || !state.mounted || !state.controllers[hand].active) {
            g_controller_vibration[hand] = 0;
        }
    }
    if (!state.session_running) {
        g_controller_history.clear();
        g_controller_sequence = 0;
    } else if (state.sample_time > g_state.sample_time) {
        ++g_controller_sequence;
        std::array<ControllerSample, 2> samples;
        for (size_t hand = 0; hand < samples.size(); ++hand) {
            samples[hand] = {updated_state.controllers[hand], state.sample_time,
                             g_controller_sequence};
        }
        g_controller_history.push_back(samples);
        if (g_controller_history.size() > 32) {
            g_controller_history.pop_front();
        }
    }
    g_state = updated_state;
}

std::array<FieldOfView, 2> GetRenderFieldOfView() {
    std::scoped_lock lock{g_mutex};
    if (!g_render_field_of_view) {
        float outer = std::atan(PsvrTanOuter);
        float inner = std::atan(PsvrTanInner);
        float top = std::atan(PsvrTanVertical);
        float bottom = top;
        if (g_state.eyes_valid) {
            const auto& left = g_state.field_of_view[0];
            const auto& right = g_state.field_of_view[1];
            outer = std::max({outer, -left.left, right.right});
            inner = std::max({inner, left.right, -right.left});
            top = std::max({top, left.up, right.up});
            bottom = std::max({bottom, -left.down, -right.down});
        }
        constexpr float margin = std::numbers::pi_v<float> / 18.0f;
        constexpr float limit = std::numbers::pi_v<float> * 4.0f / 9.0f;
        outer = std::min(outer + margin, limit);
        inner = std::min(inner + margin, limit);
        top = std::min(top + margin, limit);
        bottom = std::min(bottom + margin, limit);
        g_render_field_of_view = {{{-outer, inner, top, -bottom}, {-inner, outer, top, -bottom}}};
        LOG_INFO(Input, "VR render FOV tangents: {}, {}, {}, {} (host views: {})", std::tan(outer),
                 std::tan(inner), std::tan(top), std::tan(bottom), g_state.eyes_valid);
    }
    return *g_render_field_of_view;
}

std::optional<std::array<float, 3>> GetSeatedPadPosition() {
    std::scoped_lock lock{g_mutex};
    return g_seated_pad_position;
}

std::optional<std::array<float, 3>> GetCameraPadPosition(const std::array<float, 3>& position) {
    std::scoped_lock lock{g_mutex};
    if (!g_seated_pad_position || !g_state.mounted || !g_state.orientation_valid ||
        !std::ranges::all_of(position, [](float value) { return std::isfinite(value); })) {
        return std::nullopt;
    }
    if (!g_camera_pad_origin) {
        const auto& q = g_state.head_pose.orientation;
        auto forward = RotateToLocal({-q[0], -q[1], -q[2], q[3]}, {0.0f, 0.0f, -1.0f});
        const float length = std::hypot(forward[0], forward[2]);
        if (!std::isfinite(length) || length < 0.001f) {
            return std::nullopt;
        }
        forward[0] /= length;
        forward[2] /= length;
        g_camera_pad_forward = forward;
        g_camera_pad_origin = position;
    }
    const auto& forward = g_camera_pad_forward;
    const float dx = position[0] - g_camera_pad_origin->at(0);
    const float dy = position[1] - g_camera_pad_origin->at(1);
    const float dz = position[2] - g_camera_pad_origin->at(2);
    auto result = *g_seated_pad_position;
    result[0] += forward[2] * dx - forward[0] * dz;
    result[1] -= dy;
    result[2] -= forward[0] * dx + forward[2] * dz;
    return result;
}

bool RecenterSeatedPad() {
    std::scoped_lock lock{g_mutex};
    if (!g_state.session_running || !g_state.seated_pad ||
        g_state.controller_mode == ControllerMode::Move ||
        g_state.pad_motion_source == PadMotionSource::VrController) {
        return false;
    }
    g_seated_pad_position.reset();
    g_camera_pad_origin.reset();
    UpdateSeatedPadPosition(g_state);
    ++g_pad_recenter_sequence;
    return true;
}

std::uint64_t GetPadRecenterSequence() {
    return g_pad_recenter_sequence.load();
}

bool SetControllerVibration(std::size_t hand, std::uint8_t intensity) {
    std::scoped_lock lock{g_mutex};
    if (hand >= g_state.controllers.size() || !g_state.session_running || !g_state.mounted ||
        !g_state.controllers[hand].active) {
        return false;
    }
    g_controller_vibration[hand] = intensity;
    return true;
}

std::array<std::uint8_t, 2> GetControllerVibration() {
    std::scoped_lock lock{g_mutex};
    return g_controller_vibration;
}

std::vector<ControllerSample> GetControllerHistory(std::size_t hand) {
    std::scoped_lock lock{g_mutex};
    std::vector<ControllerSample> samples;
    if (hand >= g_state.controllers.size()) {
        return samples;
    }
    samples.reserve(g_controller_history.size());
    for (const auto& entry : g_controller_history) {
        samples.push_back(entry[hand]);
    }
    return samples;
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
