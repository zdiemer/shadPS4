// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/openxr_input.h"

#ifdef ENABLE_OPENXR

#include <cstring>
#include <string>
#include <vector>

#include "common/logging/log.h"

namespace Vulkan {

OpenXRInput::OpenXRInput(XrInstance instance_) : instance{instance_} {}

OpenXRInput::~OpenXRInput() {
    if (session != XR_NULL_HANDLE && vibration != XR_NULL_HANDLE) {
        UpdateVibration({});
    }
    for (const auto space : grip_spaces) {
        if (space != XR_NULL_HANDLE) {
            xrDestroySpace(space);
        }
    }
    for (const auto space : aim_spaces) {
        if (space != XR_NULL_HANDLE) {
            xrDestroySpace(space);
        }
    }
    if (action_set != XR_NULL_HANDLE) {
        xrDestroyActionSet(action_set);
    }
}

XrPath OpenXRInput::Path(const char* name) const {
    XrPath path{XR_NULL_PATH};
    const auto result = xrStringToPath(instance, name, &path);
    if (XR_FAILED(result)) {
        LOG_WARNING(Render_Vulkan, "Failed to resolve OpenXR path {}: {}", name,
                    static_cast<int>(result));
        return XR_NULL_PATH;
    }
    return path;
}

XrAction OpenXRInput::CreateAction(const char* name, XrActionType type) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(info.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    info.actionType = type;
    info.countSubactionPaths = hands.size();
    info.subactionPaths = hands.data();
    XrAction action{XR_NULL_HANDLE};
    const auto result = xrCreateAction(action_set, &info, &action);
    if (XR_FAILED(result)) {
        LOG_WARNING(Render_Vulkan, "Failed to create OpenXR action {}: {}", name,
                    static_cast<int>(result));
        return XR_NULL_HANDLE;
    }
    return action;
}

bool OpenXRInput::Initialize(bool frame_profile) {
    hands = {Path("/user/hand/left"), Path("/user/hand/right")};
    if (hands[0] == XR_NULL_PATH || hands[1] == XR_NULL_PATH) {
        return false;
    }
    XrActionSetCreateInfo info{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(info.actionSetName, "psvr_controls", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(info.localizedActionSetName, "PSVR Controls",
                 XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (XR_FAILED(xrCreateActionSet(instance, &info, &action_set))) {
        return false;
    }
    constexpr std::array names{"cross",    "circle",      "square", "triangle", "menu",
                               "view",     "up",          "down",   "left",     "right",
                               "shoulder", "stick_click", "select", "face_pad"};
    for (size_t i = 0; i < buttons.size(); ++i) {
        buttons[i] = CreateAction(names[i], XR_ACTION_TYPE_BOOLEAN_INPUT);
        if (buttons[i] == XR_NULL_HANDLE) {
            return false;
        }
    }
    trigger = CreateAction("trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    squeeze = CreateAction("squeeze", XR_ACTION_TYPE_FLOAT_INPUT);
    stick = CreateAction("stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    grip_pose = CreateAction("grip_pose", XR_ACTION_TYPE_POSE_INPUT);
    aim_pose = CreateAction("aim_pose", XR_ACTION_TYPE_POSE_INPUT);
    vibration = CreateAction("vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT);
    if (!trigger || !squeeze || !stick || !grip_pose || !aim_pose || !vibration) {
        return false;
    }

    const auto suggest = [&](const char* profile, bool touch, bool index, bool vive, bool frame) {
        std::vector<XrActionSuggestedBinding> bindings;
        const auto bind = [&](XrAction action, size_t hand, const char* component) {
            const std::string path =
                std::string{hand == 0 ? "/user/hand/left" : "/user/hand/right"} + component;
            const XrPath binding = Path(path.c_str());
            if (binding != XR_NULL_PATH) {
                bindings.push_back({action, binding});
            }
        };
        for (size_t hand = 0; hand < hands.size(); ++hand) {
            bind(grip_pose, hand, "/input/grip/pose");
            bind(aim_pose, hand, "/input/aim/pose");
            bind(vibration, hand, "/output/haptic");
            if (!touch && !index && !vive && !frame) {
                bind(buttons[12], hand, "/input/select/click");
                bind(buttons[4], hand, "/input/menu/click");
                continue;
            }
            bind(trigger, hand, "/input/trigger/value");
            bind(squeeze, hand, vive ? "/input/squeeze/click" : "/input/squeeze/value");
            bind(stick, hand, vive ? "/input/trackpad" : "/input/thumbstick");
            bind(buttons[vive ? 13 : 11], hand,
                 vive ? "/input/trackpad/click" : "/input/thumbstick/click");
            if (vive) {
                bind(buttons[hand == 0 ? 4 : 5], hand, "/input/menu/click");
            } else if (index) {
                bind(buttons[hand == 0 ? 4 : 5], hand, "/input/trackpad/force");
            } else if (frame) {
                bind(buttons[10], hand, "/input/shoulder/click");
                bind(buttons[hand == 0 ? 5 : 4], hand,
                     hand == 0 ? "/input/view/click" : "/input/menu/click");
            } else if (touch && hand == 0) {
                bind(buttons[4], hand, "/input/menu/click");
            }
        }
        if (touch || index || frame) {
            bind(buttons[0], 1, "/input/a/click");
            bind(buttons[1], 1, "/input/b/click");
            bind(buttons[2], frame ? 1 : 0, index ? "/input/a/click" : "/input/x/click");
            bind(buttons[3], frame ? 1 : 0, index ? "/input/b/click" : "/input/y/click");
        }
        if (frame) {
            bind(buttons[6], 0, "/input/dpad_up/click");
            bind(buttons[7], 0, "/input/dpad_down/click");
            bind(buttons[8], 0, "/input/dpad_left/click");
            bind(buttons[9], 0, "/input/dpad_right/click");
        }
        XrInteractionProfileSuggestedBinding suggested{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = Path(profile);
        suggested.countSuggestedBindings = bindings.size();
        suggested.suggestedBindings = bindings.data();
        const auto result = xrSuggestInteractionProfileBindings(instance, &suggested);
        if (XR_FAILED(result)) {
            LOG_WARNING(Render_Vulkan, "OpenXR controller profile {} rejected: {}", profile,
                        static_cast<int>(result));
        }
    };
    suggest("/interaction_profiles/oculus/touch_controller", true, false, false, false);
    suggest("/interaction_profiles/valve/index_controller", false, true, false, false);
    suggest("/interaction_profiles/htc/vive_controller", false, false, true, false);
    suggest("/interaction_profiles/khr/simple_controller", false, false, false, false);
    if (frame_profile) {
        suggest("/interaction_profiles/valve/frame_controller_valve", false, false, false, true);
    }
    return true;
}

bool OpenXRInput::Attach(XrSession session_) {
    session = session_;
    XrSessionActionSetsAttachInfo info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    info.countActionSets = 1;
    info.actionSets = &action_set;
    if (XR_FAILED(xrAttachSessionActionSets(session, &info))) {
        return false;
    }
    for (size_t hand = 0; hand < hands.size(); ++hand) {
        XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        space.subactionPath = hands[hand];
        space.poseInActionSpace.orientation.w = 1.0f;
        space.action = grip_pose;
        if (XR_FAILED(xrCreateActionSpace(session, &space, &grip_spaces[hand]))) {
            return false;
        }
        space.action = aim_pose;
        if (XR_FAILED(xrCreateActionSpace(session, &space, &aim_spaces[hand]))) {
            return false;
        }
    }
    return true;
}

void OpenXRInput::LogInteractionProfiles() {
    std::scoped_lock lock{mutex};
    for (size_t hand = 0; hand < hands.size(); ++hand) {
        XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
        if (XR_FAILED(xrGetCurrentInteractionProfile(session, hands[hand], &profile))) {
            continue;
        }
        if (profile.interactionProfile == XR_NULL_PATH) {
            LOG_INFO(Input, "OpenXR hand {} interaction profile: unbound", hand);
            continue;
        }
        std::array<char, XR_MAX_PATH_LENGTH> name{};
        std::uint32_t length{};
        if (XR_SUCCEEDED(xrPathToString(instance, profile.interactionProfile, name.size(), &length,
                                        name.data()))) {
            LOG_INFO(Input, "OpenXR hand {} interaction profile: {}", hand, name.data());
        }
    }
}

void OpenXRInput::Sync(bool focused, Input::Vr::DeviceState& state) {
    std::scoped_lock lock{mutex};
    const auto previous = state.controllers;
    state.controllers = {};
    if (!focused) {
        UpdateVibration(state);
        return;
    }
    const XrActiveActionSet active{action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (xrSyncActions(session, &sync) != XR_SUCCESS) {
        UpdateVibration(state);
        return;
    }
    for (size_t hand = 0; hand < hands.size(); ++hand) {
        auto& controller = state.controllers[hand];
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.subactionPath = hands[hand];
        get.action = grip_pose;
        XrActionStatePose pose{XR_TYPE_ACTION_STATE_POSE};
        if (XR_SUCCEEDED(xrGetActionStatePose(session, &get, &pose))) {
            controller.active = pose.isActive;
        }
        for (size_t button = 0; button < buttons.size(); ++button) {
            get.action = buttons[button];
            XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
            if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &get, &value)) && value.isActive) {
                controller.active = true;
                controller.available_buttons |= 1u << button;
                if (value.currentState) {
                    controller.buttons |= 1u << button;
                }
            }
        }
        const auto read_float = [&](XrAction action) {
            get.action = action;
            XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
            return XR_SUCCEEDED(xrGetActionStateFloat(session, &get, &value)) && value.isActive
                       ? value.currentState
                       : 0.0f;
        };
        controller.trigger = read_float(trigger);
        controller.squeeze = read_float(squeeze);
        get.action = stick;
        XrActionStateVector2f value{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &get, &value)) && value.isActive) {
            controller.stick = {value.currentState.x, value.currentState.y};
        }
        if (controller.active != previous[hand].active ||
            controller.buttons != previous[hand].buttons) {
            LOG_DEBUG(Input, "OpenXR hand {} active = {}, buttons = {:#x}", hand, controller.active,
                      controller.buttons);
        }
    }
    UpdateVibration(state);
}

void OpenXRInput::UpdateVibration(const Input::Vr::DeviceState& state) {
    const auto requested = Input::Vr::GetControllerVibration();
    const auto now = std::chrono::steady_clock::now();
    for (size_t hand = 0; hand < hands.size(); ++hand) {
        const auto intensity =
            state.mounted && state.controllers[hand].active ? requested[hand] : 0;
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action = vibration;
        info.subactionPath = hands[hand];
        if (intensity == 0) {
            if (applied_vibration[hand] != 0) {
                xrStopHapticFeedback(session, &info);
                applied_vibration[hand] = 0;
            }
            continue;
        }
        if (intensity == applied_vibration[hand] && now < vibration_refresh[hand]) {
            continue;
        }
        XrHapticVibration feedback{XR_TYPE_HAPTIC_VIBRATION};
        feedback.duration = std::chrono::nanoseconds{std::chrono::milliseconds{100}}.count();
        feedback.frequency = XR_FREQUENCY_UNSPECIFIED;
        feedback.amplitude = intensity / 255.0f;
        const auto result = xrApplyHapticFeedback(
            session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&feedback));
        if (XR_SUCCEEDED(result)) {
            applied_vibration[hand] = intensity;
            vibration_refresh[hand] = now + std::chrono::milliseconds{50};
        }
    }
}

void OpenXRInput::Locate(XrSpace local_space, XrTime time, Input::Vr::DeviceState& state) {
    std::scoped_lock lock{mutex};
    for (size_t hand = 0; hand < hands.size(); ++hand) {
        auto& controller = state.controllers[hand];
        if (!controller.active) {
            continue;
        }
        controller.position_valid = controller.orientation_valid = false;
        controller.position_tracked = controller.orientation_tracked = false;
        controller.linear_velocity_valid = controller.angular_velocity_valid = false;
        controller.aim_valid = false;
        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        location.next = &velocity;
        if (XR_FAILED(xrLocateSpace(grip_spaces[hand], local_space, time, &location))) {
            continue;
        }
        const auto convert = [](const XrPosef& pose) {
            return Input::Vr::Pose{
                .position = {pose.position.x, pose.position.y, pose.position.z},
                .orientation = {pose.orientation.x, pose.orientation.y, pose.orientation.z,
                                pose.orientation.w},
            };
        };
        controller.grip_pose = convert(location.pose);
        controller.position_valid = location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT;
        controller.orientation_valid =
            location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        controller.position_tracked =
            location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        controller.orientation_tracked =
            location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        controller.linear_velocity_valid =
            velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
        controller.angular_velocity_valid =
            velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
        controller.linear_velocity = {velocity.linearVelocity.x, velocity.linearVelocity.y,
                                      velocity.linearVelocity.z};
        controller.angular_velocity = {velocity.angularVelocity.x, velocity.angularVelocity.y,
                                       velocity.angularVelocity.z};
        location = {XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(aim_spaces[hand], local_space, time, &location))) {
            controller.aim_valid =
                (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
            controller.aim_pose = convert(location.pose);
        }
    }
}

} // namespace Vulkan

#endif
