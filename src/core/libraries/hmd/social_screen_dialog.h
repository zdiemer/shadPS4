// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/libraries/system/commondialog.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::SocialScreenDialog {

struct OrbisSocialScreenDialogParam {
    CommonDialog::BaseParam base_param;
    u64 size;
    u8 reserved[32];
};
static_assert(sizeof(OrbisSocialScreenDialogParam) == 88);

CommonDialog::Error PS4_SYSV_ABI sceSocialScreenDialogInitialize();
CommonDialog::Error PS4_SYSV_ABI
sceSocialScreenDialogOpen(const OrbisSocialScreenDialogParam* param);
CommonDialog::Error PS4_SYSV_ABI sceSocialScreenDialogClose();
CommonDialog::Error PS4_SYSV_ABI sceSocialScreenDialogGetResult(CommonDialog::Result* result);
CommonDialog::Error PS4_SYSV_ABI sceSocialScreenDialogTerminate();
CommonDialog::Status PS4_SYSV_ABI sceSocialScreenDialogGetStatus();
CommonDialog::Status PS4_SYSV_ABI sceSocialScreenDialogUpdateStatus();

void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::SocialScreenDialog
