// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/psvr_camera.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

#include <SDL3/SDL_surface.h>

namespace Input {

namespace {
struct CameraGeometry {
    int packed_width;
    int packed_height;
    int width;
    int height;
};

constexpr std::array Geometries{
    CameraGeometry{3448, 808, 1280, 800},
    CameraGeometry{1748, 408, 640, 400},
    CameraGeometry{898, 200, 320, 192},
};
constexpr int ImageOffset = 96;

std::optional<SDL_CameraSpec> FindFormat(SDL_CameraID device, int width, int framerate) {
    const auto geometry = std::ranges::find(Geometries, width, &CameraGeometry::width);
    if (geometry == Geometries.end()) {
        return std::nullopt;
    }
    int count{};
    auto** formats = SDL_GetCameraSupportedFormats(device, &count);
    std::optional<SDL_CameraSpec> result;
    for (int i = 0; i < count; ++i) {
        const auto& format = *formats[i];
        if (format.format == SDL_PIXELFORMAT_YUY2 && format.width == geometry->packed_width &&
            format.height == geometry->packed_height && format.framerate_denominator > 0 &&
            static_cast<s64>(format.framerate_numerator) ==
                static_cast<s64>(framerate) * format.framerate_denominator) {
            result = format;
            break;
        }
    }
    SDL_free(formats);
    return result;
}
} // namespace

PsvrCamera::~PsvrCamera() {
    Close();
}

bool PsvrCamera::IsDevice(SDL_CameraID device) {
    const auto* name = SDL_GetCameraName(device);
    return name && std::strstr(name, "OV580") && FindFormat(device, 1280, 60).has_value();
}

bool PsvrCamera::Open(SDL_CameraID device, int image_width, int framerate) {
    std::scoped_lock lock{mutex};
    if (camera) {
        return SDL_SetError("PSVR camera is already open");
    }
    if (!IsDevice(device)) {
        return SDL_SetError("Selected device is not a supported PSVR stereo camera");
    }
    const auto format = FindFormat(device, image_width, framerate);
    if (!format) {
        return SDL_SetError("PSVR camera does not advertise the requested stereo mode");
    }
    camera = SDL_OpenCamera(device, &*format);
    if (!camera) {
        return false;
    }
    const auto geometry = std::ranges::find(Geometries, image_width, &CameraGeometry::width);
    width = geometry->width;
    height = geometry->height;
    sequence = 0;
    latest_frame.reset();
    return true;
}

void PsvrCamera::Close() {
    std::scoped_lock lock{mutex};
    if (camera) {
        SDL_CloseCamera(std::exchange(camera, nullptr));
    }
    latest_frame.reset();
}

std::shared_ptr<const StereoCameraFrame> PsvrCamera::ReadFrame() {
    std::scoped_lock lock{mutex};
    if (!camera || SDL_GetCameraPermissionState(camera) != 1) {
        return {};
    }
    for (int i = 0; i < 8; ++i) {
        u64 timestamp{};
        auto* frame = SDL_AcquireCameraFrame(camera, &timestamp);
        if (!frame) {
            break;
        }
        const auto geometry = std::ranges::find(Geometries, width, &CameraGeometry::width);
        if (frame->format == SDL_PIXELFORMAT_YUY2 && frame->w == geometry->packed_width &&
            frame->h == geometry->packed_height && frame->pitch >= ImageOffset + width * 4) {
            auto result = std::make_shared<StereoCameraFrame>();
            result->width = width;
            result->height = height;
            result->timestamp_ns = timestamp;
            result->sequence = ++sequence;
            result->sample_time = std::chrono::steady_clock::now();
            for (size_t channel = 0; channel < result->images.size(); ++channel) {
                auto& image = result->images[channel];
                image.resize(static_cast<size_t>(width) * height * 2);
                for (int y = 0; y < height; ++y) {
                    const auto* source = static_cast<const u8*>(frame->pixels) +
                                         static_cast<size_t>(y) * frame->pitch + ImageOffset +
                                         channel * width * 2;
                    std::memcpy(image.data() + static_cast<size_t>(y) * width * 2, source,
                                width * 2);
                }
            }
            latest_frame = std::move(result);
        }
        SDL_ReleaseCameraFrame(camera, frame);
    }
    if (!latest_frame || std::chrono::steady_clock::now() - latest_frame->sample_time >
                             std::chrono::milliseconds{250}) {
        return {};
    }
    return latest_frame;
}

} // namespace Input
