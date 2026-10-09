// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/hmd/hmd.h"
#include "core/libraries/hmd/hmd_setup_dialog.h"
#include "core/libraries/libs.h"
#include "input/vr_state.h"

namespace Libraries::HmdSetupDialog {

using CommonDialog::Error;
using CommonDialog::Status;

static std::mutex g_mutex;
static auto g_status = Status::NONE;
static OrbisHmdSetupDialogResult g_result{};
static Libraries::UserService::OrbisUserServiceUserId g_setup_user_id =
    Libraries::UserService::ORBIS_USER_SERVICE_USER_ID_INVALID;

s32 PS4_SYSV_ABI sceHmdSetupDialogInitialize() {
    std::scoped_lock lock{g_mutex};
    if (g_status != Status::NONE) {
        return static_cast<s32>(Error::ALREADY_INITIALIZED);
    }
    g_status = Status::INITIALIZED;
    g_result = {};
    LOG_DEBUG(Lib_HmdSetupDialog, "initialized");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogClose() {
    std::scoped_lock lock{g_mutex};
    if (g_status == Status::NONE) {
        return static_cast<s32>(Error::NOT_INITIALIZED);
    }
    if (g_status != Status::RUNNING) {
        return static_cast<s32>(Error::NOT_RUNNING);
    }
    g_result.result = CommonDialog::Result::USER_CANCELED;
    g_status = Status::FINISHED;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogOpen(const OrbisHmdSetupDialogParam* param) {
    std::scoped_lock lock{g_mutex};
    if (g_status == Status::NONE) {
        return static_cast<s32>(Error::NOT_INITIALIZED);
    }
    if (g_status == Status::RUNNING) {
        return static_cast<s32>(Error::BUSY);
    }
    if (param == nullptr) {
        return static_cast<s32>(Libraries::CommonDialog::Error::ARG_NULL);
    }
    if (param->size != sizeof(OrbisHmdSetupDialogParam) ||
        param->base_param.size != sizeof(CommonDialog::BaseParam) ||
        param->user_id == Libraries::UserService::ORBIS_USER_SERVICE_USER_ID_INVALID ||
        param->user_id == Libraries::UserService::ORBIS_USER_SERVICE_USER_ID_SYSTEM) {
        return static_cast<s32>(Libraries::CommonDialog::Error::PARAM_INVALID);
    }
    LOG_DEBUG(Lib_HmdSetupDialog, "user_id = {}, size = {}", param->user_id, param->size);
    g_setup_user_id = param->user_id;
    g_result = {};
    g_status = Status::RUNNING;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogGetResult(OrbisHmdSetupDialogResult* result) {
    std::scoped_lock lock{g_mutex};
    LOG_DEBUG(Lib_HmdSetupDialog, "called");
    if (g_status == Status::NONE) {
        return static_cast<s32>(Error::NOT_INITIALIZED);
    }
    if (g_status != Status::FINISHED) {
        return static_cast<s32>(Error::NOT_FINISHED);
    }
    if (result == nullptr) {
        return static_cast<s32>(Libraries::CommonDialog::Error::ARG_NULL);
    }
    *result = g_result;
    return ORBIS_OK;
}

Libraries::CommonDialog::Status PS4_SYSV_ABI sceHmdSetupDialogUpdateStatus() {
    std::scoped_lock lock{g_mutex};
    if (g_status == Status::RUNNING) {
        const auto state = Input::Vr::GetDeviceState();
        if (state.connected && state.session_running) {
            Hmd::BindDeviceToUser(g_setup_user_id);
            g_result.result = CommonDialog::Result::OK;
            g_status = Status::FINISHED;
            LOG_DEBUG(Lib_HmdSetupDialog, "HMD setup completed");
        } else if (!state.connected) {
            g_result.result = CommonDialog::Result::USER_CANCELED;
            g_status = Status::FINISHED;
        }
    }
    return g_status;
}

Libraries::CommonDialog::Status PS4_SYSV_ABI sceHmdSetupDialogGetStatus() {
    std::scoped_lock lock{g_mutex};
    return g_status;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogTerminate() {
    std::scoped_lock lock{g_mutex};
    if (g_status == Status::NONE) {
        return static_cast<s32>(Error::NOT_INITIALIZED);
    }
    g_status = Status::NONE;
    g_result = {};
    g_setup_user_id = Libraries::UserService::ORBIS_USER_SERVICE_USER_ID_INVALID;
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("nmHzU4Gh0xs", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogClose);
    LIB_FUNCTION("6lVRHMV5LY0", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogGetResult);
    LIB_FUNCTION("J9eBpW1udl4", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogGetStatus);
    LIB_FUNCTION("NB1Y2kA2jCY", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogInitialize);
    LIB_FUNCTION("NNgiV4T+akU", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogOpen);
    LIB_FUNCTION("+z4OJmFreZc", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogTerminate);
    LIB_FUNCTION("Ud7j3+RDIBg", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogUpdateStatus);
};

} // namespace Libraries::HmdSetupDialog
