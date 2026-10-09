// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>

#include "common/logging/log.h"
#include "core/libraries/hmd/vr_service_dialog.h"
#include "core/libraries/libs.h"
#include "core/memory.h"
#include "input/vr_state.h"

namespace Libraries::VrServiceDialog {

namespace {

std::mutex g_mutex;
CommonDialog::Status g_status{CommonDialog::Status::NONE};
OrbisVrServiceDialogResult g_result{};

} // namespace

CommonDialog::Error PS4_SYSV_ABI sceVrServiceDialogInitialize() {
    std::scoped_lock lock{g_mutex};
    if (g_status != CommonDialog::Status::NONE) {
        return CommonDialog::Error::ALREADY_INITIALIZED;
    }
    g_status = CommonDialog::Status::INITIALIZED;
    g_result = {};
    LOG_DEBUG(Lib_VrServiceDialog, "initialized");
    return CommonDialog::Error::OK;
}

CommonDialog::Error PS4_SYSV_ABI sceVrServiceDialogOpen(const OrbisVrServiceDialogParam* param) {
    std::scoped_lock lock{g_mutex};
    if (g_status == CommonDialog::Status::NONE) {
        return CommonDialog::Error::NOT_INITIALIZED;
    }
    if (g_status == CommonDialog::Status::RUNNING) {
        return CommonDialog::Error::BUSY;
    }
    if (param == nullptr) {
        return CommonDialog::Error::ARG_NULL;
    }
    if (!Core::Memory::Instance()->IsValidMapping(reinterpret_cast<VAddr>(param), sizeof(*param)) ||
        param->size != sizeof(*param) ||
        param->base_param.size != sizeof(CommonDialog::BaseParam)) {
        return CommonDialog::Error::PARAM_INVALID;
    }
    if (param->mode != 0) {
        return CommonDialog::Error::NOT_SUPPORTED;
    }
    g_result = {};
    g_status = CommonDialog::Status::RUNNING;
    LOG_DEBUG(Lib_VrServiceDialog, "opened mode = {}", param->mode);
    return CommonDialog::Error::OK;
}

CommonDialog::Error PS4_SYSV_ABI sceVrServiceDialogClose() {
    std::scoped_lock lock{g_mutex};
    if (g_status == CommonDialog::Status::NONE) {
        return CommonDialog::Error::NOT_INITIALIZED;
    }
    if (g_status != CommonDialog::Status::RUNNING) {
        return CommonDialog::Error::NOT_RUNNING;
    }
    g_result.result = CommonDialog::Result::USER_CANCELED;
    g_status = CommonDialog::Status::FINISHED;
    return CommonDialog::Error::OK;
}

CommonDialog::Error PS4_SYSV_ABI sceVrServiceDialogGetResult(OrbisVrServiceDialogResult* result) {
    std::scoped_lock lock{g_mutex};
    if (g_status == CommonDialog::Status::NONE) {
        return CommonDialog::Error::NOT_INITIALIZED;
    }
    if (g_status != CommonDialog::Status::FINISHED) {
        return CommonDialog::Error::NOT_FINISHED;
    }
    if (result == nullptr) {
        return CommonDialog::Error::ARG_NULL;
    }
    if (!Core::Memory::Instance()->IsValidMapping(reinterpret_cast<VAddr>(result),
                                                  sizeof(*result))) {
        return CommonDialog::Error::PARAM_INVALID;
    }
    *result = g_result;
    LOG_DEBUG(Lib_VrServiceDialog, "result = {}", static_cast<u32>(g_result.result));
    return CommonDialog::Error::OK;
}

CommonDialog::Error PS4_SYSV_ABI sceVrServiceDialogTerminate() {
    std::scoped_lock lock{g_mutex};
    if (g_status == CommonDialog::Status::NONE) {
        return CommonDialog::Error::NOT_INITIALIZED;
    }
    g_result = {};
    g_status = CommonDialog::Status::NONE;
    LOG_DEBUG(Lib_VrServiceDialog, "terminated");
    return CommonDialog::Error::OK;
}

CommonDialog::Status PS4_SYSV_ABI sceVrServiceDialogGetStatus() {
    std::scoped_lock lock{g_mutex};
    return g_status;
}

CommonDialog::Status PS4_SYSV_ABI sceVrServiceDialogUpdateStatus() {
    std::scoped_lock lock{g_mutex};
    if (g_status == CommonDialog::Status::RUNNING) {
        const auto state = Input::Vr::GetDeviceState();
        if (state.connected && state.session_running) {
            g_result.result = CommonDialog::Result::OK;
            g_status = CommonDialog::Status::FINISHED;
            LOG_DEBUG(Lib_VrServiceDialog, "VR service ready");
        } else if (!state.connected) {
            g_result.result = CommonDialog::Result::USER_CANCELED;
            g_status = CommonDialog::Status::FINISHED;
        }
    }
    return g_status;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("hYFXG8FWThI", "libSceVrServiceDialog", 1, "libSceVrServiceDialog",
                 sceVrServiceDialogInitialize);
    LIB_FUNCTION("60-cjn5Dn0Q", "libSceVrServiceDialog", 1, "libSceVrServiceDialog",
                 sceVrServiceDialogOpen);
    LIB_FUNCTION("hBH2ABP7IeY", "libSceVrServiceDialog", 1, "libSceVrServiceDialog",
                 sceVrServiceDialogClose);
    LIB_FUNCTION("cYnBkgm8I0c", "libSceVrServiceDialog", 1, "libSceVrServiceDialog",
                 sceVrServiceDialogGetResult);
    LIB_FUNCTION("M4xKWUytNMo", "libSceVrServiceDialog", 1, "libSceVrServiceDialog",
                 sceVrServiceDialogTerminate);
    LIB_FUNCTION("kUavKmsczkY", "libSceVrServiceDialog", 1, "libSceVrServiceDialog",
                 sceVrServiceDialogGetStatus);
    LIB_FUNCTION("RmRtBJpoHlA", "libSceVrServiceDialog", 1, "libSceVrServiceDialog",
                 sceVrServiceDialogUpdateStatus);
}

} // namespace Libraries::VrServiceDialog
