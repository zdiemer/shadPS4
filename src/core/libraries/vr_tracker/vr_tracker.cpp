// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <optional>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/libraries/camera/vr_camera.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/libraries/move/move.h"
#include "core/libraries/vr_tracker/vr_tracker.h"
#include "core/libraries/vr_tracker/vr_tracker_error.h"
#include "core/memory.h"
#include "input/controller.h"
#include "input/vr_state.h"
#include "video_core/amdgpu/liverpool.h"

namespace Libraries::VrTracker {

static bool g_library_initialized = false;
static std::mutex g_mutex;
enum class FrameState { Idle, Submitted, Waited, Processed };
static FrameState g_frame_state{FrameState::Idle};
static std::optional<u64> g_camera_timestamp;
static u32 g_camera_frame{};
static OrbisVrTrackerDevicePermitType g_camera_permit{ORBIS_VR_TRACKER_DEVICE_PERMIT_ALL};
enum class CalibrationState { Idle, Requested, Sampling };
static constexpr auto CalibrationSamplingDuration = std::chrono::milliseconds{100};
struct Calibration {
    CalibrationState state{CalibrationState::Idle};
    OrbisVrTrackerCalibrationType type{ORBIS_VR_TRACKER_CALIBRATION_POSITION};
    std::optional<u32> camera_frame;
    std::chrono::steady_clock::time_point sampling_started{};
};
static std::array<Calibration, 4> g_calibration;
static std::array<float, 4> g_relative_orientation{0.0f, 0.0f, 0.0f, 1.0f};
static std::array<float, 4> g_pad_relative_orientation{0.0f, 0.0f, 0.0f, 1.0f};
static std::array<float, 4> g_pad_motion_relative_orientation{0.0f, 0.0f, 0.0f, 1.0f};
static std::array<std::array<float, 4>, 2> g_move_relative_orientation{
    {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}}};

// Internal memory
static void* g_garlic_memory_pointer = nullptr;
static u32 g_garlic_size = 0;
static void* g_onion_memory_pointer = nullptr;
static u32 g_onion_size = 0;
static void* g_work_memory_pointer = nullptr;
static u32 g_work_size = 0;

// Registered handles
static s32 g_pad_handle = -1;
static OrbisVrTrackerLedColor g_pad_led_color{ORBIS_VR_TRACKER_LED_COLOR_BLUE};
static std::array<s32, 2> g_move_handles{-1, -1};
static s32 g_gun_handle = -1;
static s32 g_hmd_handle = -1;

static std::optional<Input::State> GetPadMotionState() {
    auto* controller = Pad::GetController(g_pad_handle);
    return controller ? controller->ReadMotionState() : std::nullopt;
}

static bool HasPadVrInput() {
    const auto& controllers = *Common::Singleton<Input::GameControllers>::Instance();
    return Pad::GetController(g_pad_handle) == controllers[0];
}

static void AdvanceCalibration() {
    const auto state = Input::Vr::GetDeviceState();
    const auto controller_ready = [&](size_t index, const Calibration& calibration) {
        const auto& controller = state.controllers[index];
        return controller.active && controller.position_valid &&
               (calibration.type == ORBIS_VR_TRACKER_CALIBRATION_POSITION ||
                controller.orientation_valid);
    };
    for (size_t device = 0; device < g_calibration.size(); ++device) {
        auto& calibration = g_calibration[device];
        if (calibration.state == CalibrationState::Idle ||
            calibration.camera_frame == g_camera_frame) {
            continue;
        }
        calibration.camera_frame = g_camera_frame;
        bool ready = state.session_running;
        switch (device) {
        case ORBIS_VR_TRACKER_DEVICE_HMD:
            ready &= state.connected && state.position_valid &&
                     (calibration.type == ORBIS_VR_TRACKER_CALIBRATION_POSITION ||
                      state.orientation_valid);
            break;
        case ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4:
            ready &= HasPadVrInput() && !GetPadMotionState() &&
                     g_camera_permit == ORBIS_VR_TRACKER_DEVICE_PERMIT_ALL &&
                     controller_ready(state.controllers[1].active ? 1 : 0, calibration);
            break;
        case ORBIS_VR_TRACKER_DEVICE_MOVE:
            ready &= g_camera_permit == ORBIS_VR_TRACKER_DEVICE_PERMIT_ALL;
            for (const auto handle : g_move_handles) {
                if (handle == -1) {
                    continue;
                }
                const auto index = Move::GetControllerIndex(handle);
                ready &= index && controller_ready(*index, calibration);
            }
            break;
        default:
            ready = false;
            break;
        }
        const auto previous_state = calibration.state;
        if (!ready) {
            calibration.state = CalibrationState::Requested;
        } else if (calibration.state == CalibrationState::Requested) {
            calibration.sampling_started = state.sample_time;
            calibration.state = CalibrationState::Sampling;
        } else if (state.sample_time - calibration.sampling_started >=
                   CalibrationSamplingDuration) {
            calibration.state = CalibrationState::Idle;
        }
        if (calibration.state != previous_state) {
            LOG_DEBUG(Lib_VrTracker, "Calibration device {}, state {}, camera frame {}", device,
                      static_cast<u32>(calibration.state), g_camera_frame);
        }
    }
}

static s32 WaitForTrackingFrame() {
    if (g_frame_state == FrameState::Idle) {
        return ORBIS_VR_TRACKER_ERROR_NOT_EXECUTE_GPU_SUBMIT;
    }
    if (g_frame_state == FrameState::Submitted) {
        g_frame_state = FrameState::Waited;
    }
    return ORBIS_OK;
}

static s32 ProcessTrackingFrame() {
    if (g_frame_state == FrameState::Idle || g_frame_state == FrameState::Submitted) {
        return ORBIS_VR_TRACKER_ERROR_NOT_EXECUTE_GPU_WAIT;
    }
    if (g_frame_state != FrameState::Processed) {
        AdvanceCalibration();
        g_frame_state = FrameState::Processed;
    }
    return ORBIS_OK;
}

static OrbisVrTrackerPoseData ConvertPose(
    const Input::Vr::Pose& pose, bool relative,
    const std::array<float, 4>& origin = g_relative_orientation) {
    auto orientation = pose.orientation;
    if (relative) {
        orientation = Input::Vr::RelativeOrientation(pose.orientation, origin);
    }
    return {
        .position_x = pose.position[0],
        .position_y = pose.position[1],
        .position_z = pose.position[2],
        .orientation_x = orientation[0],
        .orientation_y = orientation[1],
        .orientation_z = orientation[2],
        .orientation_w = orientation[3],
    };
}

s32 PS4_SYSV_ABI sceVrTrackerQueryMemory(const OrbisVrTrackerQueryMemoryParam* param,
                                         OrbisVrTrackerQueryMemoryResult* result) {
    LOG_DEBUG(Lib_VrTracker, "called");
    if (param == nullptr || result == nullptr ||
        param->size != sizeof(OrbisVrTrackerQueryMemoryParam) ||
        (param->profile != OrbisVrTrackerProfile::ORBIS_VR_TRACKER_PROFILE_000 &&
         param->profile != OrbisVrTrackerProfile::ORBIS_VR_TRACKER_PROFILE_100) ||
        param->calibration_settings.pad_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        // Hmd doesn't support auto calibration
        param->calibration_settings.hmd_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_MANUAL ||
        param->calibration_settings.move_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        param->calibration_settings.gun_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Setting move_position to ORBIS_VR_TRACKER_CALIBRATION_AUTO doubles required onion memory.
    u32 required_onion_size = ORBIS_VR_TRACKER_BASE_ONION_SIZE;
    if (param->calibration_settings.move_position ==
        OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        required_onion_size *= 2;
    }

    result->direct_memory_onion_size = required_onion_size;
    result->direct_memory_onion_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;
    result->direct_memory_garlic_size = ORBIS_VR_TRACKER_GARLIC_SIZE;
    result->direct_memory_garlic_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;
    result->work_memory_size = ORBIS_VR_TRACKER_WORK_SIZE;
    result->work_memory_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerInit(const OrbisVrTrackerInitParam* param) {
    std::scoped_lock lock{g_mutex};
    if (g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_ALREADY_INITIALIZED;
    }

    if (param == nullptr) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    LOG_DEBUG(Lib_VrTracker, "Initialize size {}, profile {}, execution mode {}", param->size,
              static_cast<s32>(param->profile), static_cast<s32>(param->execution_mode));

    OrbisVrTrackerInitParam normalized_param{};
    if (param->size == sizeof(OrbisVrTrackerInitParam144)) {
        const auto& supplied = *reinterpret_cast<const OrbisVrTrackerInitParam144*>(param);
        normalized_param = {
            .size = sizeof(OrbisVrTrackerInitParam),
            .profile = supplied.profile,
            .execution_mode = supplied.execution_mode,
            .hmd_thread_priority = supplied.hmd_thread_priority,
            .pad_thread_priority = supplied.pad_thread_priority,
            .move_thread_priority = supplied.move_thread_priority,
            .gun_thread_priority = supplied.gun_thread_priority,
            .calibration_settings = supplied.calibration_settings,
            .direct_memory_onion = supplied.direct_memory_onion,
            .direct_memory_onion_size = supplied.direct_memory_onion_size,
            .direct_memory_onion_alignment = supplied.direct_memory_onion_alignment,
            .direct_memory_garlic = supplied.direct_memory_garlic,
            .direct_memory_garlic_size = supplied.direct_memory_garlic_size,
            .direct_memory_garlic_alignment = supplied.direct_memory_garlic_alignment,
            .work_memory = supplied.work_memory,
            .work_memory_size = supplied.work_memory_size,
            .work_memory_alignment = supplied.work_memory_alignment,
            .gpu_pipe_id = supplied.gpu_pipe_id,
            .gpu_queue_id = supplied.gpu_queue_id,
        };
        param = &normalized_param;
    }

    // Calculate correct onion size for parameter checks
    u32 required_onion_size = ORBIS_VR_TRACKER_BASE_ONION_SIZE;
    if (param->calibration_settings.move_position == ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        required_onion_size *= 2;
    }

    // Parameter checks are fairly thorough here.
    if (param->size != sizeof(OrbisVrTrackerInitParam) ||
        (param->execution_mode != ORBIS_VR_TRACKER_EXECUTION_MODE_SERIAL &&
         param->execution_mode != ORBIS_VR_TRACKER_EXECUTION_MODE_PARALLEL) ||
        // Check garlic memory parameters
        param->direct_memory_garlic == nullptr ||
        param->direct_memory_garlic_alignment != ORBIS_VR_TRACKER_MEMORY_ALIGNMENT ||
        param->direct_memory_garlic_size != ORBIS_VR_TRACKER_GARLIC_SIZE ||
        // Check onion memory parameters
        param->direct_memory_onion == nullptr ||
        param->direct_memory_onion_alignment != ORBIS_VR_TRACKER_MEMORY_ALIGNMENT ||
        param->direct_memory_onion_size != required_onion_size ||
        // Check work memory parameters
        param->work_memory == nullptr ||
        param->work_memory_alignment != ORBIS_VR_TRACKER_MEMORY_ALIGNMENT ||
        param->work_memory_size != ORBIS_VR_TRACKER_WORK_SIZE ||
        // Check compute queue parameters
        param->gpu_pipe_id >= AmdGpu::Liverpool::NumComputePipes ||
        param->gpu_queue_id >= AmdGpu::Liverpool::NumQueuesPerPipe ||
        // Check calibration settings
        param->calibration_settings.pad_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        param->calibration_settings.hmd_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_MANUAL ||
        param->calibration_settings.move_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        param->calibration_settings.gun_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Real hardware will segfault if any of the supplied mappings aren't long enough,
    // Validate each of them to ensure nothing weird can occur when this library is implemented.
    auto* memory = Core::Memory::Instance();
    Libraries::Kernel::OrbisVirtualQueryInfo info;
    // The memory type for the whole range should be the same here,
    // so the memory should be contained in one VMA.
    VAddr addr_to_check = std::bit_cast<VAddr>(param->direct_memory_garlic);
    s32 result = memory->VirtualQuery(addr_to_check, 0, &info);
    ASSERT_MSG(result == 0 && info.end - addr_to_check >= param->direct_memory_garlic_size,
               "Insufficient garlic memory provided");

    g_garlic_memory_pointer = param->direct_memory_garlic;
    g_garlic_size = param->direct_memory_garlic_size;

    addr_to_check = std::bit_cast<VAddr>(param->direct_memory_onion);
    result = memory->VirtualQuery(addr_to_check, 0, &info);
    ASSERT_MSG(result == 0 && info.end - addr_to_check >= param->direct_memory_onion_size,
               "Insufficient onion memory provided");

    g_onion_memory_pointer = param->direct_memory_onion;
    g_onion_size = param->direct_memory_onion_size;

    addr_to_check = std::bit_cast<VAddr>(param->work_memory);
    result = memory->VirtualQuery(addr_to_check, 0, &info);
    ASSERT_MSG(result == 0 && info.end - addr_to_check >= param->work_memory_size,
               "Insufficient work memory provided");

    g_work_memory_pointer = param->work_memory;
    g_work_size = param->work_memory_size;

    // All initialization checks passed.
    LOG_WARNING(Lib_VrTracker, "PSVR camera processing is not implemented");
    g_library_initialized = true;
    g_frame_state = FrameState::Idle;
    g_camera_timestamp.reset();
    g_calibration.fill({});
    g_pad_led_color = ORBIS_VR_TRACKER_LED_COLOR_BLUE;
    g_relative_orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    g_pad_relative_orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    g_pad_motion_relative_orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    g_move_relative_orientation.fill({0.0f, 0.0f, 0.0f, 1.0f});

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerRegisterDevice(const OrbisVrTrackerDeviceType device_type,
                                            const s32 handle) {
    LOG_TRACE(Lib_VrTracker, "redirected to sceVrTrackerRegisterDeviceInternal");
    return sceVrTrackerRegisterDeviceInternal(device_type, handle, -1, 0);
}

s32 PS4_SYSV_ABI sceVrTrackerRegisterDevice2(const OrbisVrTrackerDeviceType device_type,
                                             const s32 handle) {
    LOG_TRACE(Lib_VrTracker, "redirected to sceVrTrackerRegisterDeviceInternal");
    return sceVrTrackerRegisterDeviceInternal(device_type, handle, -1, 1);
}

s32 PS4_SYSV_ABI sceVrTrackerRegisterDeviceInternal(const OrbisVrTrackerDeviceType device_type,
                                                    const s32 handle, s32 unk0, s32 unk1) {
    std::scoped_lock lock{g_mutex};
    LOG_WARNING(Lib_VrTracker, "(STUBBED) called, device_type = {}, handle = {}",
                static_cast<u32>(device_type), handle);
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (device_type < ORBIS_VR_TRACKER_DEVICE_HMD || device_type > ORBIS_VR_TRACKER_DEVICE_GUN ||
        handle < 0 || unk0 > 4) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Ignore handle handle validation for now, since most of that logic isn't really handled.
    switch (device_type) {
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_HMD: {
        if (g_hmd_handle != -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        }
        g_hmd_handle = handle;
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4: {
        if (g_pad_handle != -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        }
        g_pad_handle = handle;
        g_pad_led_color =
            unk0 < 0 ? ORBIS_VR_TRACKER_LED_COLOR_BLUE : static_cast<OrbisVrTrackerLedColor>(unk0);
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_MOVE: {
        if (std::ranges::find(g_move_handles, handle) != g_move_handles.end()) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        }
        const auto slot = std::ranges::find(g_move_handles, -1);
        if (slot == g_move_handles.end()) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_LIMIT;
        }
        *slot = handle;
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN: {
        if (g_gun_handle != -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        }
        g_gun_handle = handle;
        break;
    }
    default: {
        // Shouldn't be possible to hit this.
        UNREACHABLE();
    }
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerCpuProcess(const OrbisVrTrackerCpuProcessParam* param) {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (param == nullptr || param->size != sizeof(*param) ||
        (param->operation_mode != ORBIS_VR_TRACKER_CPU_PROCESS_OPERATION_MODE_WHOLE &&
         param->operation_mode != ORBIS_VR_TRACKER_CPU_PROCESS_OPERATION_MODE_HANDLE)) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    if (param->operation_mode == ORBIS_VR_TRACKER_CPU_PROCESS_OPERATION_MODE_HANDLE) {
        if (param->handle < 0) {
            return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
        }
        if (param->handle != g_hmd_handle && param->handle != g_pad_handle &&
            param->handle != g_gun_handle &&
            std::ranges::find(g_move_handles, param->handle) == g_move_handles.end()) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
    }
    return ProcessTrackingFrame();
}

s32 PS4_SYSV_ABI sceVrTrackerGetPlayAreaWarningInfo(OrbisVrTrackerPlayAreaWarningInfo* info) {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (info == nullptr || info->size != sizeof(*info)) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    *info = {.size = sizeof(*info)};
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGetResult(const OrbisVrTrackerGetResultParam* param,
                                       OrbisVrTrackerResultData* result) {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (param == nullptr || result == nullptr || param->size != sizeof(*param) ||
        (param->result_type != ORBIS_VR_TRACKER_RESULT_RAW &&
         param->result_type != ORBIS_VR_TRACKER_RESULT_PREDICTED) ||
        (param->orientation_type != ORBIS_VR_TRACKER_ORIENTATION_ABSOLUTE &&
         param->orientation_type != ORBIS_VR_TRACKER_ORIENTATION_RELATIVE) ||
        (param->usage_type != ORBIS_VR_TRACKER_USAGE_DEFAULT &&
         param->usage_type != ORBIS_VR_TRACKER_USAGE_OPTIMIZED_FOR_HMD_USER) ||
        param->debug_marker_type < ORBIS_VR_TRACKER_DEBUG_MARKER_UNSPECIFIED ||
        param->debug_marker_type > ORBIS_VR_TRACKER_DEBUG_MARKER_OTHER || param->handle < 0) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    const auto move = std::ranges::find(g_move_handles, param->handle);
    const bool move_registered = move != g_move_handles.end();
    if (param->handle != g_hmd_handle && param->handle != g_pad_handle && !move_registered &&
        param->handle != g_gun_handle) {
        return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
    }
    const auto now = std::chrono::steady_clock::now();
    const u64 process_time = Kernel::sceKernelGetProcessTime();
    u64 requested_time = process_time;
    if (param->result_type == ORBIS_VR_TRACKER_RESULT_PREDICTED) {
        requested_time = param->prediction_time;
    }
    if (requested_time > static_cast<u64>(std::numeric_limits<s64>::max()) ||
        process_time > static_cast<u64>(std::numeric_limits<s64>::max())) {
        return ORBIS_VR_TRACKER_ERROR_TIMESTAMP_OUT_OF_RANGE;
    }
    const s64 offset = static_cast<s64>(requested_time) - static_cast<s64>(process_time);
    const auto max_offset = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::time_point::max() - now)
                                .count();
    if (offset > max_offset || offset < -static_cast<s64>(process_time)) {
        return ORBIS_VR_TRACKER_ERROR_TIMESTAMP_OUT_OF_RANGE;
    }
    *result = {};
    result->handle = param->handle;
    result->timestamp = requested_time;
    result->user_frame_number = param->user_frame_number;
    result->camera_orientation_w = 1.0f;
    result->status = ORBIS_VR_TRACKER_STATUS_NOT_TRACKING;
    if (param->handle == g_pad_handle) {
        result->led_color = g_pad_led_color;
    }
    if (param->handle != g_hmd_handle) {
        if (move_registered) {
            result->move_info.device_pose = ConvertPose({}, false);
        } else {
            result->pad_info.device_pose = ConvertPose({}, false);
        }
        if (!move_registered && param->handle != g_pad_handle) {
            return ORBIS_OK;
        }
        if (!move_registered) {
            if (const auto motion = GetPadMotionState()) {
                result->connected = true;
                result->device_timestamp = motion->time;
                result->status = g_calibration[ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4].state ==
                                         CalibrationState::Idle
                                     ? ORBIS_VR_TRACKER_STATUS_TRACKING
                                     : ORBIS_VR_TRACKER_STATUS_CALIBRATING;
                result->orientation_quality = ORBIS_VR_TRACKER_QUALITY_PARTIAL;
                const auto& q = motion->orientation;
                const Input::Vr::Pose pose{.orientation = {q.x, q.y, q.z, q.w}};
                result->pad_info.device_pose = ConvertPose(
                    pose, param->orientation_type == ORBIS_VR_TRACKER_ORIENTATION_RELATIVE,
                    g_pad_motion_relative_orientation);
                const auto velocity = Input::Vr::RotateToLocal(
                    {-q.x, -q.y, -q.z, q.w}, {motion->angularVelocity.x, motion->angularVelocity.y,
                                              motion->angularVelocity.z});
                result->angular_velocity_x = velocity[0];
                result->angular_velocity_y = velocity[1];
                result->angular_velocity_z = velocity[2];
                return ORBIS_OK;
            }
        }
        const auto state = Input::Vr::LocateDevice(now + std::chrono::microseconds{offset});
        if (!state) {
            return ORBIS_OK;
        }
        const auto index =
            move_registered
                ? Move::GetControllerIndex(param->handle)
                : (HasPadVrInput() ? std::optional<size_t>{state->controllers[1].active ? 1 : 0}
                                   : std::nullopt);
        if (!index) {
            return ORBIS_OK;
        }
        const auto& controller = state->controllers[*index];
        result->connected = controller.active;
        if (!controller.active) {
            return ORBIS_OK;
        }
        const auto device =
            move_registered ? ORBIS_VR_TRACKER_DEVICE_MOVE : ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4;
        if (g_calibration[device].state != CalibrationState::Idle) {
            result->status = ORBIS_VR_TRACKER_STATUS_CALIBRATING;
            return ORBIS_OK;
        }
        if (controller.orientation_valid || controller.position_valid) {
            result->status = ORBIS_VR_TRACKER_STATUS_TRACKING;
        }
        result->position_quality =
            controller.position_valid
                ? (controller.position_tracked ? ORBIS_VR_TRACKER_QUALITY_FULL
                                               : ORBIS_VR_TRACKER_QUALITY_PARTIAL)
                : ORBIS_VR_TRACKER_QUALITY_NONE;
        result->orientation_quality =
            controller.orientation_valid
                ? (controller.orientation_tracked ? ORBIS_VR_TRACKER_QUALITY_FULL
                                                  : ORBIS_VR_TRACKER_QUALITY_PARTIAL)
                : ORBIS_VR_TRACKER_QUALITY_NONE;
        const auto& origin = move_registered
                                 ? g_move_relative_orientation[move - g_move_handles.begin()]
                                 : g_pad_relative_orientation;
        const auto pose =
            ConvertPose(controller.grip_pose,
                        param->orientation_type == ORBIS_VR_TRACKER_ORIENTATION_RELATIVE, origin);
        if (move_registered) {
            result->move_info.device_pose = pose;
        } else {
            result->pad_info.device_pose = pose;
        }
        if (controller.linear_velocity_valid) {
            result->velocity_x = controller.linear_velocity[0];
            result->velocity_y = controller.linear_velocity[1];
            result->velocity_z = controller.linear_velocity[2];
        }
        if (controller.angular_velocity_valid) {
            result->angular_velocity_x = controller.angular_velocity[0];
            result->angular_velocity_y = controller.angular_velocity[1];
            result->angular_velocity_z = controller.angular_velocity[2];
        }
        return ORBIS_OK;
    }
    auto& hmd = result->hmd_info;
    hmd.device_pose = hmd.head_pose = hmd.left_eye_pose = hmd.right_eye_pose =
        ConvertPose({}, false);
    const auto state = Input::Vr::LocateDevice(now + std::chrono::microseconds{offset});
    if (!state) {
        result->connected = Input::Vr::GetDeviceState().connected;
        return ORBIS_OK;
    }
    result->connected = state->connected;
    hmd.sensor_read_system_timestamp = process_time;
    if (state->connected &&
        g_calibration[ORBIS_VR_TRACKER_DEVICE_HMD].state != CalibrationState::Idle) {
        result->status = ORBIS_VR_TRACKER_STATUS_CALIBRATING;
        return ORBIS_OK;
    }
    if (state->orientation_valid || state->position_valid) {
        result->status = ORBIS_VR_TRACKER_STATUS_TRACKING;
    }
    result->position_quality = state->position_valid
                                   ? (state->position_tracked ? ORBIS_VR_TRACKER_QUALITY_FULL
                                                              : ORBIS_VR_TRACKER_QUALITY_PARTIAL)
                                   : ORBIS_VR_TRACKER_QUALITY_NONE;
    result->orientation_quality =
        state->orientation_valid ? (state->orientation_tracked ? ORBIS_VR_TRACKER_QUALITY_FULL
                                                               : ORBIS_VR_TRACKER_QUALITY_PARTIAL)
                                 : ORBIS_VR_TRACKER_QUALITY_NONE;
    const bool relative = param->orientation_type == ORBIS_VR_TRACKER_ORIENTATION_RELATIVE;
    hmd.head_pose = hmd.device_pose = ConvertPose(state->head_pose, relative);
    if (state->eyes_valid) {
        hmd.left_eye_pose = ConvertPose(state->eye_poses[0], relative);
        hmd.right_eye_pose = ConvertPose(state->eye_poses[1], relative);
    } else {
        hmd.left_eye_pose = hmd.right_eye_pose = hmd.head_pose;
    }
    if (state->linear_velocity_valid) {
        result->velocity_x = state->linear_velocity[0];
        result->velocity_y = state->linear_velocity[1];
        result->velocity_z = state->linear_velocity[2];
    }
    if (state->angular_velocity_valid) {
        result->angular_velocity_x = state->angular_velocity[0];
        result->angular_velocity_y = state->angular_velocity[1];
        result->angular_velocity_z = state->angular_velocity[2];
    }
    LOG_DEBUG(Lib_VrTracker, "handle = {}, timestamp = {}, status = {}, quality = {}/{}",
              param->handle, requested_time, static_cast<u32>(result->status),
              static_cast<u32>(result->position_quality),
              static_cast<u32>(result->orientation_quality));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGetTime(u64* time) {
    std::scoped_lock lock{g_mutex};
    LOG_TRACE(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (time == nullptr) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    *time = Libraries::Kernel::sceKernelGetProcessTime();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGpuSubmit(const OrbisVrTrackerGpuSubmitParam* param) {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        LOG_DEBUG(Lib_VrTracker, "Tracking submission before initialization");
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }

    constexpr auto legacy_size = offsetof(OrbisVrTrackerGpuSubmitParam, camera_frame_data) +
                                 offsetof(Camera::OrbisCameraFrameData, pFramePointerListGarlic);
    if (param == nullptr || (param->size != sizeof(*param) && param->size != legacy_size) ||
        (param->pad_tracking_preference != ORBIS_VR_TRACKER_PREFERENCE_FAR_POSITION &&
         param->pad_tracking_preference != ORBIS_VR_TRACKER_PREFERENCE_STABLE_POSITION) ||
        (param->camera_meta_check_mode != ORBIS_VR_TRACKER_CAMERA_META_CHECK_ENABLE &&
         param->camera_meta_check_mode != ORBIS_VR_TRACKER_CAMERA_META_CHECK_DISABLE) ||
        (param->tracking_device_permit_type != ORBIS_VR_TRACKER_DEVICE_PERMIT_ALL &&
         param->tracking_device_permit_type != ORBIS_VR_TRACKER_DEVICE_PERMIT_HMD_ONLY) ||
        (param->robustness_level != ORBIS_VR_TRACKER_ROBUSTNESS_LEVEL_HIGH &&
         param->robustness_level != ORBIS_VR_TRACKER_ROBUSTNESS_LEVEL_LOW &&
         param->robustness_level != ORBIS_VR_TRACKER_ROBUSTNESS_LEVEL_MEDIUM &&
         param->robustness_level != ORBIS_VR_TRACKER_ROBUSTNESS_LEVEL_LEGACY)) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    const auto& frame = param->camera_frame_data;
    if (frame.sizeThis != param->size - offsetof(OrbisVrTrackerGpuSubmitParam, camera_frame_data)) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    if (!Camera::IsVrCameraActive() || !Input::Vr::GetDeviceState().session_running) {
        LOG_DEBUG(Lib_VrTracker, "Tracking submission without an active virtual camera session");
        return ORBIS_VR_TRACKER_ERROR_PLAYSTATION_CAMERA_NOT_CONNECTED;
    }
    for (u32 channel = 0; channel < Camera::ORBIS_CAMERA_MAX_DEVICE_NUM; ++channel) {
        if (frame.status[channel] != 1) {
            return ORBIS_VR_TRACKER_ERROR_INVALID_STATUS_OF_CAMERA_FRAME;
        }
        bool has_image = false;
        for (u32 level = 0; level < Camera::ORBIS_CAMERA_MAX_FORMAT_LEVEL_NUM; ++level) {
            has_image |= frame.pFramePointerList[channel][level] != nullptr &&
                         frame.frameSize[channel][level] != 0;
        }
        if (!has_image) {
            return ORBIS_VR_TRACKER_ERROR_INVALID_CAMERA_CONFIGURATION;
        }
    }
    if (g_frame_state == FrameState::Submitted || g_frame_state == FrameState::Waited) {
        return ORBIS_VR_TRACKER_ERROR_BUSY;
    }
    if (g_camera_timestamp && frame.meta.timestamp[0] <= *g_camera_timestamp) {
        return ORBIS_VR_TRACKER_ERROR_ALREADY_PROCESSING_CAMERA_FRAME;
    }
    g_camera_timestamp = frame.meta.timestamp[0];
    g_camera_frame = frame.meta.frame[0];
    g_camera_permit = param->tracking_device_permit_type;
    g_frame_state = FrameState::Submitted;
    LOG_DEBUG(Lib_VrTracker, "Submitted virtual camera frame {}, timestamp {}, permit {}",
              frame.meta.frame[0], *g_camera_timestamp,
              static_cast<u32>(param->tracking_device_permit_type));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGpuWait(const OrbisVrTrackerGpuWaitParam* param) {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (param == nullptr || param->size != sizeof(OrbisVrTrackerGpuWaitParam)) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    return WaitForTrackingFrame();
}

s32 PS4_SYSV_ABI sceVrTrackerGpuWaitAndCpuProcess() {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }

    const s32 result = WaitForTrackingFrame();
    return result == ORBIS_OK ? ProcessTrackingFrame() : result;
}

s32 PS4_SYSV_ABI
sceVrTrackerNotifyEndOfCpuProcess(const OrbisVrTrackerNotifyEndOfCpuProcessParam* param) {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (param == nullptr || param->size != sizeof(*param)) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    if (g_frame_state != FrameState::Processed) {
        return ORBIS_VR_TRACKER_ERROR_NOT_EXECUTE_CPU_PROCESS;
    }
    g_frame_state = FrameState::Idle;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerRecalibrate(const OrbisVrTrackerRecalibrateParam* param) {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (param == nullptr || param->size != sizeof(OrbisVrTrackerRecalibrateParam) ||
        param->device_type < ORBIS_VR_TRACKER_DEVICE_HMD ||
        param->device_type > ORBIS_VR_TRACKER_DEVICE_GUN ||
        (param->calibration_type != ORBIS_VR_TRACKER_CALIBRATION_POSITION &&
         param->calibration_type != ORBIS_VR_TRACKER_CALIBRATION_ALL)) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    OrbisVrTrackerDeviceType device_type = param->device_type;
    switch (device_type) {
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_HMD: {
        if (g_hmd_handle == -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4: {
        if (g_pad_handle == -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_MOVE: {
        if (std::ranges::find_if(g_move_handles, [](s32 handle) { return handle != -1; }) ==
            g_move_handles.end()) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN: {
        if (g_gun_handle == -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        break;
    }
    default: {
        // Shouldn't be possible to hit this.
        UNREACHABLE();
    }
    }

    g_calibration[device_type] = {
        .state = CalibrationState::Requested,
        .type = param->calibration_type,
        .camera_frame = g_camera_timestamp ? std::optional{g_camera_frame} : std::nullopt,
    };
    LOG_DEBUG(Lib_VrTracker, "Requested calibration device {}, type {}",
              static_cast<u32>(device_type), static_cast<u32>(param->calibration_type));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerResetAll() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerResetOrientationRelative(const OrbisVrTrackerDeviceType device_type,
                                                      const s32 handle) {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (device_type < ORBIS_VR_TRACKER_DEVICE_HMD || device_type > ORBIS_VR_TRACKER_DEVICE_GUN ||
        handle < 0) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    if (device_type == ORBIS_VR_TRACKER_DEVICE_GUN) {
        return ORBIS_VR_TRACKER_ERROR_NOT_SUPPORTED;
    }
    const auto move = std::ranges::find(g_move_handles, handle);
    if ((device_type == ORBIS_VR_TRACKER_DEVICE_HMD && handle != g_hmd_handle) ||
        (device_type == ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4 && handle != g_pad_handle) ||
        (device_type == ORBIS_VR_TRACKER_DEVICE_MOVE && move == g_move_handles.end())) {
        return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
    }
    if (device_type == ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4) {
        if (const auto motion = GetPadMotionState()) {
            const auto& q = motion->orientation;
            g_pad_motion_relative_orientation = {q.x, q.y, q.z, q.w};
            return ORBIS_OK;
        }
    }
    const auto state = Input::Vr::LocateDevice(std::chrono::steady_clock::now());
    if (!state) {
        return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_ORIENTED;
    }
    if (device_type != ORBIS_VR_TRACKER_DEVICE_HMD) {
        const auto index =
            device_type == ORBIS_VR_TRACKER_DEVICE_MOVE
                ? Move::GetControllerIndex(handle)
                : (HasPadVrInput() ? std::optional<size_t>{state->controllers[1].active ? 1 : 0}
                                   : std::nullopt);
        if (!index) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_ORIENTED;
        }
        const auto& controller = state->controllers[*index];
        if (!controller.active || !controller.orientation_valid) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_ORIENTED;
        }
        auto& origin = device_type == ORBIS_VR_TRACKER_DEVICE_MOVE
                           ? g_move_relative_orientation[move - g_move_handles.begin()]
                           : g_pad_relative_orientation;
        origin = controller.grip_pose.orientation;
    } else {
        if (!state->orientation_valid) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_ORIENTED;
        }
        g_relative_orientation = state->head_pose.orientation;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSaveInternalBuffers() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetDurationUntilStatusNotTracking(
    const OrbisVrTrackerDeviceType device_type, const u32 duration_camera_frames) {
    std::scoped_lock lock{g_mutex};
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (device_type > OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Seems to unconditionally return 0 when parameters are valid.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetExtendedMode() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetLEDBrightness() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetRestingMode() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI
sceVrTrackerUpdateMotionSensorData(const OrbisVrTrackerUpdateMotionSensorDataParam* param) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_0FA4C949F8D3024E() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_285C6AFC09C42F7E() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_9A6CDB2103664F8A() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerRegisterDeviceFor4thDS4(const OrbisVrTrackerDeviceType device_type,
                                                     const s32 handle, const s32 led_color) {
    return sceVrTrackerRegisterDeviceInternal(device_type, handle, led_color, 1);
}

s32 PS4_SYSV_ABI sceVrTrackerSetDeviceRejection() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_1119B0BE399F37E7() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_4928B43816BC440D() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_863EF32EFCB0FA9C() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_E6E726CBC85C48F9() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_F6407E46C66DF383() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerCpuPopMarker() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerCpuPushMarker() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGetLiveCaptureId() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerStartLiveCapture() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerStopLiveCapture() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerUnregisterDevice(const s32 handle) {
    std::scoped_lock lock{g_mutex};
    LOG_DEBUG(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (handle < 0) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    // Since this function only takes a handle, compare the handle to registered handles.
    if (handle == g_hmd_handle) {
        g_hmd_handle = -1;
        g_calibration[ORBIS_VR_TRACKER_DEVICE_HMD] = {};
    } else if (handle == g_pad_handle) {
        g_pad_handle = -1;
        g_calibration[ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4] = {};
        g_pad_led_color = ORBIS_VR_TRACKER_LED_COLOR_BLUE;
        g_pad_relative_orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        g_pad_motion_relative_orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    } else if (const auto move = std::ranges::find(g_move_handles, handle);
               move != g_move_handles.end()) {
        g_move_relative_orientation[move - g_move_handles.begin()] = {0.0f, 0.0f, 0.0f, 1.0f};
        *move = -1;
        if (std::ranges::all_of(g_move_handles, [](s32 handle) { return handle == -1; })) {
            g_calibration[ORBIS_VR_TRACKER_DEVICE_MOVE] = {};
        }
    } else if (handle == g_gun_handle) {
        g_gun_handle = -1;
        g_calibration[ORBIS_VR_TRACKER_DEVICE_GUN] = {};
    } else {
        // If none of the handles match up, then return an error.
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerTerm() {
    std::scoped_lock lock{g_mutex};
    LOG_DEBUG(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    g_library_initialized = false;
    g_frame_state = FrameState::Idle;
    g_camera_timestamp.reset();
    g_calibration.fill({});
    g_hmd_handle = g_pad_handle = g_gun_handle = -1;
    g_pad_led_color = ORBIS_VR_TRACKER_LED_COLOR_BLUE;
    g_move_handles.fill(-1);
    g_relative_orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    g_pad_relative_orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    g_pad_motion_relative_orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    g_move_relative_orientation.fill({0.0f, 0.0f, 0.0f, 1.0f});
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("24kDA+A0Ox0", "libSceVrTrackerFourDeviceAllowed", 1, "libSceVrTracker",
                 sceVrTrackerRegisterDevice2);
    LIB_FUNCTION("5IFOAYv-62g", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerCpuProcess);
    LIB_FUNCTION("zvyKP0Z3UvU", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerGetPlayAreaWarningInfo);
    LIB_FUNCTION("76OBvrrQXUc", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGetResult);
    LIB_FUNCTION("XoeWzXlrnMw", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGetTime);
    LIB_FUNCTION("TVegDMLaBB8", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGpuSubmit);
    LIB_FUNCTION("gkGuO9dd57M", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGpuWait);
    LIB_FUNCTION("ARhgpXvwoR0", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerGpuWaitAndCpuProcess);
    LIB_FUNCTION("QkRl7pART9M", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerInit);
    LIB_FUNCTION("VItTwN8DmS8", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerNotifyEndOfCpuProcess);
    LIB_FUNCTION("K7yhYrsIBPc", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerQueryMemory);
    LIB_FUNCTION("EUCaQtXXYNI", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerRecalibrate);
    LIB_FUNCTION("sIh8GwcevaQ", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerRegisterDevice);
    LIB_FUNCTION("ufexf4aNiwg", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerRegisterDeviceInternal);
    LIB_FUNCTION("CtWUbFgmq+I", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerResetAll);
    LIB_FUNCTION("E0P0sN-wy+4", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerResetOrientationRelative);
    LIB_FUNCTION("bDGZVTwwZ1A", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSaveInternalBuffers);
    LIB_FUNCTION("qBjnR0HtMYI", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetDurationUntilStatusNotTracking);
    LIB_FUNCTION("NhPkY3V8E+8", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetExtendedMode);
    LIB_FUNCTION("vpsLLotiSUg", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetLEDBrightness);
    LIB_FUNCTION("lgWSHQ8p4i4", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetRestingMode);
    LIB_FUNCTION("IBv4P3q1pQ0", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerTerm);
    LIB_FUNCTION("Q8skQqEwn5c", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerUnregisterDevice);
    LIB_FUNCTION("9fvHMUbsom4", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerUpdateMotionSensorData);
    LIB_FUNCTION("D6TJSfjTAk4", "libSceVrTracker", 1, "libSceVrTracker", Func_0FA4C949F8D3024E);
    LIB_FUNCTION("KFxq-AnEL34", "libSceVrTracker", 1, "libSceVrTracker", Func_285C6AFC09C42F7E);
    LIB_FUNCTION("mmzbIQNmT4o", "libSceVrTracker", 1, "libSceVrTracker", Func_9A6CDB2103664F8A);
    LIB_FUNCTION("tNJrfYsY3wY", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerRegisterDeviceFor4thDS4);
    LIB_FUNCTION("jGqEkPy0iLU", "libSceVrTrackerDeviceRejection", 1, "libSceVrTracker",
                 sceVrTrackerSetDeviceRejection);
    LIB_FUNCTION("ERmwvjmfN+c", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_1119B0BE399F37E7);
    LIB_FUNCTION("SSi0OBa8RA0", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_4928B43816BC440D);
    LIB_FUNCTION("hj7zLvyw+pw", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_863EF32EFCB0FA9C);
    LIB_FUNCTION("5ucmy8hcSPk", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_E6E726CBC85C48F9);
    LIB_FUNCTION("9kB+RsZt84M", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_F6407E46C66DF383);
    LIB_FUNCTION("sBkAqyF5Gns", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerCpuPopMarker);
    LIB_FUNCTION("rvCywCbc7Pk", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerCpuPushMarker);
    LIB_FUNCTION("lm6T1Ur6JRk", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerGetLiveCaptureId);
    LIB_FUNCTION("qa1+CeXKDPc", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerStartLiveCapture);
    LIB_FUNCTION("3YCwwpHkHIg", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerStopLiveCapture);
};

} // namespace Libraries::VrTracker
