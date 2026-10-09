// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>

#include "common/logging/log.h"
#include "core/libraries/hmd/social_screen_dialog.h"
#include "core/libraries/libs.h"
#include "core/memory.h"

namespace Libraries::SocialScreenDialog {

namespace {

std::mutex g_mutex;
CommonDialog::Status g_status{CommonDialog::Status::NONE};
CommonDialog::Result g_result{};

} // namespace

CommonDialog::Error PS4_SYSV_ABI sceSocialScreenDialogInitialize() {
    std::scoped_lock lock{g_mutex};
    if (g_status != CommonDialog::Status::NONE) {
        return CommonDialog::Error::ALREADY_INITIALIZED;
    }
    g_status = CommonDialog::Status::INITIALIZED;
    g_result = {};
    return CommonDialog::Error::OK;
}

CommonDialog::Error PS4_SYSV_ABI
sceSocialScreenDialogOpen(const OrbisSocialScreenDialogParam* param) {
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
    g_result = {};
    g_status = CommonDialog::Status::RUNNING;
    LOG_DEBUG(Lib_Hmd, "Social screen dialog opened");
    return CommonDialog::Error::OK;
}

CommonDialog::Error PS4_SYSV_ABI sceSocialScreenDialogClose() {
    std::scoped_lock lock{g_mutex};
    if (g_status == CommonDialog::Status::NONE) {
        return CommonDialog::Error::NOT_INITIALIZED;
    }
    if (g_status != CommonDialog::Status::RUNNING) {
        return CommonDialog::Error::NOT_RUNNING;
    }
    g_result = CommonDialog::Result::USER_CANCELED;
    g_status = CommonDialog::Status::FINISHED;
    return CommonDialog::Error::OK;
}

CommonDialog::Error PS4_SYSV_ABI sceSocialScreenDialogGetResult(CommonDialog::Result* result) {
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
    return CommonDialog::Error::OK;
}

CommonDialog::Error PS4_SYSV_ABI sceSocialScreenDialogTerminate() {
    std::scoped_lock lock{g_mutex};
    if (g_status == CommonDialog::Status::NONE) {
        return CommonDialog::Error::NOT_INITIALIZED;
    }
    g_status = CommonDialog::Status::NONE;
    g_result = {};
    return CommonDialog::Error::OK;
}

CommonDialog::Status PS4_SYSV_ABI sceSocialScreenDialogGetStatus() {
    std::scoped_lock lock{g_mutex};
    return g_status;
}

CommonDialog::Status PS4_SYSV_ABI sceSocialScreenDialogUpdateStatus() {
    std::scoped_lock lock{g_mutex};
    if (g_status == CommonDialog::Status::RUNNING) {
        g_status = CommonDialog::Status::FINISHED;
        LOG_DEBUG(Lib_Hmd, "Social screen dialog completed");
    }
    return g_status;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("PEe0R7gBHbc", "libSceSocialScreenDialog", 1, "libSceSocialScreenDialog",
                 sceSocialScreenDialogInitialize);
    LIB_FUNCTION("eL54zY-B+-Y", "libSceSocialScreenDialog", 1, "libSceSocialScreenDialog",
                 sceSocialScreenDialogOpen);
    LIB_FUNCTION("4ej3RtYH320", "libSceSocialScreenDialog", 1, "libSceSocialScreenDialog",
                 sceSocialScreenDialogClose);
    LIB_FUNCTION("WjHT5TmV0TQ", "libSceSocialScreenDialog", 1, "libSceSocialScreenDialog",
                 sceSocialScreenDialogGetResult);
    LIB_FUNCTION("A4KPdcTIVuc", "libSceSocialScreenDialog", 1, "libSceSocialScreenDialog",
                 sceSocialScreenDialogTerminate);
    LIB_FUNCTION("3foHkmhq6ak", "libSceSocialScreenDialog", 1, "libSceSocialScreenDialog",
                 sceSocialScreenDialogGetStatus);
    LIB_FUNCTION("BEhMn+TyoGA", "libSceSocialScreenDialog", 1, "libSceSocialScreenDialog",
                 sceSocialScreenDialogUpdateStatus);
}

} // namespace Libraries::SocialScreenDialog
