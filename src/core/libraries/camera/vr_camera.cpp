// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "core/emulator_settings.h"
#include "core/libraries/camera/camera_helpers.h"
#include "core/libraries/camera/vr_camera.h"
#include "core/libraries/kernel/time.h"
#include "input/psvr_camera.h"
#include "input/vr_state.h"

namespace Libraries::Camera {

namespace {
std::mutex g_vr_camera_mutex;
std::array<std::array<u8*, VrCameraBufferLevels>, 2> g_buffers{};
std::array<u32, 2> g_levels{};
bool g_started{};
std::atomic<bool> g_opened{};
std::array<u32, 2> g_auto_exposure{};
std::array<u32, 2> g_auto_white_balance{};
std::array<OrbisCameraWhiteBalance, 2> g_white_balance{};
std::array<OrbisCameraExposureGain, 2> g_exposure{};
Input::PsvrCamera g_physical_camera;
bool g_physical_started{};
u64 g_physical_sequence{};
u64 g_physical_timestamp{};

std::optional<SDL_CameraID> GetPhysicalCamera() {
    const auto index = EmulatorSettings.GetCameraId();
    if (index < 0) {
        return std::nullopt;
    }
    int count{};
    auto* devices = SDL_GetCameras(&count);
    std::optional<SDL_CameraID> result;
    if (index < count && Input::PsvrCamera::IsDevice(devices[index])) {
        result = devices[index];
    }
    SDL_free(devices);
    return result;
}

void CopyPhysicalPlane(const Input::StereoCameraFrame& frame, u32 channel, u32 width, u32 height,
                       u32 level, u32 format, u8* buffer) {
    const auto& source = frame.images[channel];
    const bool raw = level == 0 && format != ORBIS_CAMERA_FORMAT_YUV422;
    std::vector<u8> scaled;
    if (raw) {
        scaled.resize(static_cast<size_t>(width) * height * 2);
    }
    auto* target = raw ? scaled.data() : buffer;
    for (u32 y = 0; y < height; ++y) {
        const u32 source_y = y * frame.height / height;
        for (u32 x = 0; x < width; x += 2) {
            const auto* row = source.data() + source_y * frame.width * 2;
            const u32 source_x = x * frame.width / width;
            const u32 next_x = std::min(x + 1, width - 1) * frame.width / width;
            const auto* pair = row + (source_x & ~1u) * 2;
            const u8 first = row[source_x * 2];
            const u8 second = row[next_x * 2];
            const u32 offset = y * width + x;
            if (raw || format == ORBIS_CAMERA_SCALE_FORMAT_YUV422) {
                target[offset * 2] = first;
                target[offset * 2 + 1] = pair[1];
                if (x + 1 < width) {
                    target[offset * 2 + 2] = second;
                    target[offset * 2 + 3] = pair[3];
                }
            } else if (format == ORBIS_CAMERA_SCALE_FORMAT_Y16) {
                auto* output = reinterpret_cast<u16*>(target);
                output[offset] = static_cast<u16>(first) << 2;
                if (x + 1 < width) {
                    output[offset + 1] = static_cast<u16>(second) << 2;
                }
            } else {
                target[offset] = first;
                if (x + 1 < width) {
                    target[offset + 1] = second;
                }
            }
        }
    }
    if (raw && format == ORBIS_CAMERA_FORMAT_RAW16) {
        ConvertYUY2ToRAW16(scaled.data(), reinterpret_cast<u16*>(buffer), width, height);
    } else if (raw) {
        ConvertYUY2ToRAW8(scaled.data(), buffer, width, height);
    }
}

std::pair<u32, u32> GetDimensions(const OrbisCameraConfigExtention& config) {
    switch (config.resolution) {
    case ORBIS_CAMERA_RESOLUTION_1280X800:
        return {1280, 800};
    case ORBIS_CAMERA_RESOLUTION_640X400:
        return {640, 400};
    case ORBIS_CAMERA_RESOLUTION_320X200:
        return {320, 200};
    case ORBIS_CAMERA_RESOLUTION_160X100:
        return {160, 100};
    case ORBIS_CAMERA_RESOLUTION_320X192:
        return {320, 192};
    case ORBIS_CAMERA_RESOLUTION_SPECIFIED_WIDTH_HEIGHT:
        return {config.width, config.height};
    default:
        return {};
    }
}
} // namespace

bool IsVrCameraAvailable() {
    return Input::Vr::GetDeviceState().connected &&
           (EmulatorSettings.GetCameraId() == -1 || GetPhysicalCamera().has_value());
}

bool IsVrCameraActive() {
    return g_opened;
}

void OpenVrCamera() {
    std::scoped_lock lock{g_vr_camera_mutex};
    g_opened = true;
    g_started = false;
    output_config0 = camera_config_types[0][0];
    output_config1 = camera_config_types[0][1];
    g_auto_exposure = {};
    g_auto_white_balance = {};
    g_white_balance.fill({0, 768, 768, 512});
    g_exposure.fill({0, 83, 100, 0});
}

void CloseVrCamera() {
    std::scoped_lock lock{g_vr_camera_mutex};
    g_started = false;
    g_opened = false;
    g_physical_camera.Close();
    g_physical_started = false;
}

void InitializeVrCameraBuffers(u8* memory) {
    std::scoped_lock lock{g_vr_camera_mutex};
    for (auto& channel : g_buffers) {
        for (u32 level = 0; level < channel.size(); ++level) {
            channel[level] = memory;
            memory += (1280 >> level) * (800 >> level) * 2;
        }
    }
}

s32 ConfigureVrCamera(const OrbisCameraConfigExtention& first,
                      const OrbisCameraConfigExtention& second) {
    std::scoped_lock lock{g_vr_camera_mutex};
    if (!IsVrCameraAvailable()) {
        return ORBIS_CAMERA_ERROR_NOT_CONNECTED;
    }
    if (g_started) {
        return ORBIS_CAMERA_ERROR_ALREADY_START;
    }
    for (const auto& config : {first, second}) {
        const auto [width, height] = GetDimensions(config);
        if (width == 0 || height == 0 || width > 1280 || height > 800 || width % 2 != 0) {
            return ORBIS_CAMERA_ERROR_RESOLUTION_UNKNOWN;
        }
        if (config.framerate != ORBIS_CAMERA_FRAMERATE_7_5 &&
            config.framerate != ORBIS_CAMERA_FRAMERATE_15 &&
            config.framerate != ORBIS_CAMERA_FRAMERATE_30 &&
            config.framerate != ORBIS_CAMERA_FRAMERATE_60 &&
            config.framerate != ORBIS_CAMERA_FRAMERATE_120 &&
            config.framerate != ORBIS_CAMERA_FRAMERATE_240) {
            return ORBIS_CAMERA_ERROR_BAD_FRAMERATE;
        }
        if (config.format.formatLevel0 > ORBIS_CAMERA_FORMAT_RAW8 &&
            config.format.formatLevel0 != ORBIS_CAMERA_FORMAT_NO_USE) {
            return ORBIS_CAMERA_ERROR_FORMAT_UNKNOWN;
        }
        for (const auto format :
             {config.format.formatLevel1, config.format.formatLevel2, config.format.formatLevel3}) {
            if (format != ORBIS_CAMERA_SCALE_FORMAT_YUV422 &&
                format != ORBIS_CAMERA_SCALE_FORMAT_Y16 && format != ORBIS_CAMERA_SCALE_FORMAT_Y8 &&
                format != ORBIS_CAMERA_SCALE_FORMAT_NO_USE) {
                return ORBIS_CAMERA_ERROR_FORMAT_UNKNOWN;
            }
        }
    }
    output_config0 = first;
    output_config1 = second;
    return ORBIS_OK;
}

OrbisCameraConfig GetVrCameraConfig() {
    std::scoped_lock lock{g_vr_camera_mutex};
    return {
        sizeof(OrbisCameraConfig), ORBIS_CAMERA_CONFIG_EXTENTION, {output_config0, output_config1}};
}

s32 SetVrCameraAutoExposure(OrbisCameraChannel channel, u32 enable) {
    std::scoped_lock lock{g_vr_camera_mutex};
    for (u32 index = 0; index < 2; ++index) {
        if (channel == ORBIS_CAMERA_CHANNEL_BOTH || channel == index + 1) {
            g_auto_exposure[index] = enable;
        }
    }
    return ORBIS_OK;
}

u32 GetVrCameraAutoExposure(OrbisCameraChannel channel) {
    std::scoped_lock lock{g_vr_camera_mutex};
    return g_auto_exposure[channel - 1];
}

void SetVrCameraAutoWhiteBalance(OrbisCameraChannel channel, u32 enable) {
    std::scoped_lock lock{g_vr_camera_mutex};
    for (u32 index = 0; index < 2; ++index) {
        if (channel == ORBIS_CAMERA_CHANNEL_BOTH || channel == index + 1) {
            g_auto_white_balance[index] = enable;
        }
    }
}

u32 GetVrCameraAutoWhiteBalance(OrbisCameraChannel channel) {
    std::scoped_lock lock{g_vr_camera_mutex};
    return g_auto_white_balance[channel - 1];
}

void SetVrCameraExposure(OrbisCameraChannel channel, const OrbisCameraExposureGain& value) {
    std::scoped_lock lock{g_vr_camera_mutex};
    for (u32 index = 0; index < 2; ++index) {
        if (channel == ORBIS_CAMERA_CHANNEL_BOTH || channel == index + 1) {
            g_exposure[index] = value;
        }
    }
}

OrbisCameraExposureGain GetVrCameraExposure(OrbisCameraChannel channel) {
    std::scoped_lock lock{g_vr_camera_mutex};
    return g_exposure[channel - 1];
}

void SetVrCameraWhiteBalance(OrbisCameraChannel channel, const OrbisCameraWhiteBalance& value) {
    std::scoped_lock lock{g_vr_camera_mutex};
    for (u32 index = 0; index < 2; ++index) {
        if (channel == ORBIS_CAMERA_CHANNEL_BOTH || channel == index + 1) {
            g_white_balance[index] = value;
        }
    }
}

OrbisCameraWhiteBalance GetVrCameraWhiteBalance(OrbisCameraChannel channel) {
    std::scoped_lock lock{g_vr_camera_mutex};
    return g_white_balance[channel - 1];
}

s32 StartVrCamera(const OrbisCameraStartParameter& param) {
    std::scoped_lock lock{g_vr_camera_mutex};
    if (!IsVrCameraAvailable()) {
        return ORBIS_CAMERA_ERROR_NOT_CONNECTED;
    }
    if (g_started) {
        return ORBIS_CAMERA_ERROR_ALREADY_START;
    }
    if (param.pStartOption != nullptr || param.formatLevel[0] > 15 || param.formatLevel[1] > 15 ||
        (param.formatLevel[0] | param.formatLevel[1]) == 0) {
        return ORBIS_CAMERA_ERROR_PARAM;
    }
    if (const auto device = GetPhysicalCamera()) {
        if (!g_physical_camera.Open(*device)) {
            return ORBIS_CAMERA_ERROR_FATAL;
        }
        g_physical_started = true;
        g_physical_sequence = 0;
        g_physical_timestamp = 0;
    }
    g_levels = {param.formatLevel[0], param.formatLevel[1]};
    g_started = true;
    return ORBIS_OK;
}

s32 StopVrCamera() {
    std::scoped_lock lock{g_vr_camera_mutex};
    if (!g_started) {
        return ORBIS_CAMERA_ERROR_NOT_START;
    }
    g_started = false;
    g_physical_camera.Close();
    g_physical_started = false;
    return ORBIS_OK;
}

s32 ReadVrCamera(OrbisCameraFrameData* frame_data) {
    std::scoped_lock lock{g_vr_camera_mutex};
    if (!g_started) {
        return ORBIS_CAMERA_ERROR_NOT_START;
    }
    if (!IsVrCameraAvailable()) {
        return ORBIS_CAMERA_ERROR_NOT_CONNECTED;
    }
    const u32 size = frame_data->sizeThis;
    if (size != sizeof(OrbisCameraFrameData) &&
        size != offsetof(OrbisCameraFrameData, pFramePointerListGarlic)) {
        return ORBIS_CAMERA_ERROR_PARAM;
    }
    OrbisCameraFrameData result{.sizeThis = size, .readMode = frame_data->readMode};
    const u64 time = Kernel::sceKernelGetProcessTime();
    std::shared_ptr<const Input::StereoCameraFrame> physical_frame;
    if (g_physical_started) {
        physical_frame = g_physical_camera.ReadFrame();
        if (!physical_frame) {
            return ORBIS_CAMERA_ERROR_BUSY;
        }
        if (physical_frame->sequence != g_physical_sequence) {
            g_physical_sequence = physical_frame->sequence;
            g_physical_timestamp = time;
        }
    }
    const std::array configs{output_config0, output_config1};
    for (u32 channel = 0; channel < configs.size(); ++channel) {
        const auto& config = configs[channel];
        const auto [width, height] = GetDimensions(config);
        const std::array formats{static_cast<u32>(config.format.formatLevel0),
                                 static_cast<u32>(config.format.formatLevel1),
                                 static_cast<u32>(config.format.formatLevel2),
                                 static_cast<u32>(config.format.formatLevel3)};
        result.meta.frame[channel] = config.framerate == ORBIS_CAMERA_FRAMERATE_7_5
                                         ? time * 15 / 2000000
                                         : time * static_cast<u32>(config.framerate) / 1000000;
        result.meta.timestamp[channel] = time;
        result.meta.deviceTimestamp[channel] = static_cast<u32>(time);
        if (physical_frame) {
            result.meta.frame[channel] = g_physical_sequence;
            result.meta.timestamp[channel] = g_physical_timestamp;
            result.meta.deviceTimestamp[channel] =
                static_cast<u32>(physical_frame->timestamp_ns / 1000);
        }
        result.meta.exposureGain[channel] = g_exposure[channel];
        result.meta.whiteBalance[channel] = g_white_balance[channel];
        for (u32 level = 0; level < formats.size(); ++level) {
            result.meta.format[channel][level] = formats[level];
            if ((g_levels[channel] & (1u << level)) == 0 || formats[level] == 0x10) {
                continue;
            }
            const u32 bytes_per_pixel =
                level == 0 ? SizeOfBaseFormat(config.format.formatLevel0)
                           : SizeOfScaleFormat(static_cast<OrbisCameraScaleFormat>(formats[level]));
            const u32 level_width = width >> level;
            const u32 level_height = height >> level;
            const u32 byte_size = level_width * level_height * bytes_per_pixel;
            auto* buffer = g_buffers[channel][level];
            std::memset(buffer, 0, byte_size);
            if (physical_frame) {
                CopyPhysicalPlane(*physical_frame, channel, level_width, level_height, level,
                                  formats[level], buffer);
            } else if (formats[level] == 0) {
                for (u32 offset = 0; offset < byte_size; offset += 4) {
                    const std::array<u8, 4> black{16, 128, 16, 128};
                    std::memcpy(buffer + offset, black.data(),
                                std::min<u32>(4, byte_size - offset));
                }
            }
            result.framePosition[channel][level] = {0, 0, level_width, level_height};
            result.frameSize[channel][level] = byte_size;
            result.pFramePointerList[channel][level] = buffer;
            result.pFramePointerListGarlic[channel][level] = buffer;
            result.status[channel] = 1;
        }
    }
    result.meta.acceleration_y = -9.80665f;
    result.meta.vcounter = result.meta.frame[0];
    std::memcpy(frame_data, &result, size);
    return ORBIS_OK;
}

void GetVrCameraCalibration(const OrbisCameraGetCalibrationDataParameter& param,
                            OrbisCameraCalibrationData* data) {
    *data = {.format_type = param.format_type, .function_type = param.function_type};
    for (auto& element : data->data) {
        element.total_horizontal_verticies = 44;
        element.total_vertical_verticies = 29;
        for (u32 y = 0; y < 29; ++y) {
            for (u32 x = 0; x < 44; ++x) {
                const u32 index = y * 44 + x;
                element.x_table[index] = static_cast<float>(x) / 43;
                element.y_table[index] = static_cast<float>(y) / 28;
            }
        }
    }
}

} // namespace Libraries::Camera
