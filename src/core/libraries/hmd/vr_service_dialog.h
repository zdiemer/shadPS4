// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/libraries/system/commondialog.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::VrServiceDialog {

struct OrbisVrServiceDialogParam {
    CommonDialog::BaseParam base_param;
    u64 size;
    u32 mode;
    u8 reserved[44];
};
static_assert(sizeof(OrbisVrServiceDialogParam) == 104);

struct OrbisVrServiceDialogResult {
    CommonDialog::Result result;
    u8 reserved[32];
};
static_assert(sizeof(OrbisVrServiceDialogResult) == 36);

CommonDialog::Error PS4_SYSV_ABI sceVrServiceDialogInitialize();
CommonDialog::Error PS4_SYSV_ABI sceVrServiceDialogOpen(const OrbisVrServiceDialogParam* param);
CommonDialog::Error PS4_SYSV_ABI sceVrServiceDialogClose();
CommonDialog::Error PS4_SYSV_ABI sceVrServiceDialogGetResult(OrbisVrServiceDialogResult* result);
CommonDialog::Error PS4_SYSV_ABI sceVrServiceDialogTerminate();
CommonDialog::Status PS4_SYSV_ABI sceVrServiceDialogGetStatus();
CommonDialog::Status PS4_SYSV_ABI sceVrServiceDialogUpdateStatus();

void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::VrServiceDialog
