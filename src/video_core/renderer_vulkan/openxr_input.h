// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_OPENXR

#include <array>
#include <mutex>
#include <openxr/openxr.h>
#include "input/vr_state.h"

namespace Vulkan {

class OpenXRInput {
public:
    explicit OpenXRInput(XrInstance instance);
    ~OpenXRInput();
    bool Initialize(bool frame_profile);
    bool Attach(XrSession session);
    void Sync(bool focused, Input::Vr::DeviceState& state);
    void Locate(XrSpace local_space, XrTime time, Input::Vr::DeviceState& state);

private:
    XrPath Path(const char* name) const;
    XrAction CreateAction(const char* name, XrActionType type);

    XrInstance instance;
    XrSession session{XR_NULL_HANDLE};
    XrActionSet action_set{XR_NULL_HANDLE};
    std::array<XrPath, 2> hands{};
    std::array<XrAction, 14> buttons{};
    XrAction trigger{XR_NULL_HANDLE};
    XrAction squeeze{XR_NULL_HANDLE};
    XrAction stick{XR_NULL_HANDLE};
    XrAction grip_pose{XR_NULL_HANDLE};
    XrAction aim_pose{XR_NULL_HANDLE};
    std::array<XrSpace, 2> grip_spaces{};
    std::array<XrSpace, 2> aim_spaces{};
    std::mutex mutex;
};

} // namespace Vulkan

#endif
