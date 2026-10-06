// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/hmd/hmd.h"
#include "core/libraries/hmd/hmd_setup_dialog.h"
#include "core/libraries/libs.h"
#include "input/vr_state.h"

namespace Libraries::HmdSetupDialog {

static Libraries::UserService::OrbisUserServiceUserId g_setup_user_id =
    Libraries::UserService::ORBIS_USER_SERVICE_USER_ID_INVALID;

s32 PS4_SYSV_ABI sceHmdSetupDialogInitialize() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogClose() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogOpen(const OrbisHmdSetupDialogParam* param) {
    if (param == nullptr) {
        return static_cast<s32>(Libraries::CommonDialog::Error::ARG_NULL);
    }
    if (param->size != sizeof(OrbisHmdSetupDialogParam) ||
        param->user_id == Libraries::UserService::ORBIS_USER_SERVICE_USER_ID_INVALID ||
        param->user_id == Libraries::UserService::ORBIS_USER_SERVICE_USER_ID_SYSTEM) {
        return static_cast<s32>(Libraries::CommonDialog::Error::PARAM_INVALID);
    }
    LOG_DEBUG(Lib_HmdSetupDialog, "user_id = {}, size = {}", param->user_id, param->size);
    g_setup_user_id = param->user_id;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogGetResult(OrbisHmdSetupDialogResult* result) {
    LOG_DEBUG(Lib_HmdSetupDialog, "called");
    if (result == nullptr) {
        return static_cast<s32>(Libraries::CommonDialog::Error::ARG_NULL);
    }
    *result = {};
    const auto state = Input::Vr::GetDeviceState();
    if (state.connected && state.session_running &&
        g_setup_user_id != Libraries::UserService::ORBIS_USER_SERVICE_USER_ID_INVALID) {
        Hmd::BindDeviceToUser(g_setup_user_id);
        result->result = Libraries::CommonDialog::Result::OK;
    } else {
        result->result = Libraries::CommonDialog::Result::USER_CANCELED;
    }
    return ORBIS_OK;
}

Libraries::CommonDialog::Status PS4_SYSV_ABI sceHmdSetupDialogUpdateStatus() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return Libraries::CommonDialog::Status::FINISHED;
}

Libraries::CommonDialog::Status PS4_SYSV_ABI sceHmdSetupDialogGetStatus() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return Libraries::CommonDialog::Status::FINISHED;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogTerminate() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
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
