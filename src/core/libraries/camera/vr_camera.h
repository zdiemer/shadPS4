// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/libraries/camera/camera.h"

namespace Libraries::Camera {

bool IsVrCameraAvailable();
bool IsVrCameraActive();
void OpenVrCamera();
void CloseVrCamera();
void InitializeVrCameraBuffers(u8* memory);
s32 ConfigureVrCamera(const OrbisCameraConfigExtention& first,
                      const OrbisCameraConfigExtention& second);
OrbisCameraConfig GetVrCameraConfig();
s32 SetVrCameraAutoExposure(OrbisCameraChannel channel, u32 enable);
u32 GetVrCameraAutoExposure(OrbisCameraChannel channel);
void SetVrCameraAutoWhiteBalance(OrbisCameraChannel channel, u32 enable);
u32 GetVrCameraAutoWhiteBalance(OrbisCameraChannel channel);
void SetVrCameraExposure(OrbisCameraChannel channel, const OrbisCameraExposureGain& value);
OrbisCameraExposureGain GetVrCameraExposure(OrbisCameraChannel channel);
void SetVrCameraWhiteBalance(OrbisCameraChannel channel, const OrbisCameraWhiteBalance& value);
OrbisCameraWhiteBalance GetVrCameraWhiteBalance(OrbisCameraChannel channel);
s32 StartVrCamera(const OrbisCameraStartParameter& param);
s32 StopVrCamera();
s32 ReadVrCamera(OrbisCameraFrameData* frame_data);
void GetVrCameraCalibration(const OrbisCameraGetCalibrationDataParameter& param,
                            OrbisCameraCalibrationData* data);

} // namespace Libraries::Camera
