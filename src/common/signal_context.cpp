// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include "common/arch.h"
#include "common/assert.h"
#include "common/signal_context.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__FreeBSD__)
#include <machine/npx.h>
#include <sys/ucontext.h>
#else
#include <sys/ucontext.h>
#endif

namespace Common {

void* GetRip(void* ctx) {
#if defined(_WIN32)
    return (void*)((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__rip;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__pc;
#elif defined(__FreeBSD__)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.mc_rip;
#elif defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP];
#else
#error "Unsupported architecture"
#endif
}

void IncrementRip(void* ctx, u64 length) {
#if defined(_WIN32)
    static_cast<EXCEPTION_POINTERS*>(ctx)->ContextRecord->Rip += length;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__rip += length;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__pc += length;
#elif defined(__FreeBSD__)
    static_cast<ucontext_t*>(ctx)->uc_mcontext.mc_rip += length;
#elif defined(ARCH_X86_64)
    static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_RIP] += length;
#else
#error "Unsupported architecture"
#endif
}

void SetX64Register(void* ctx, u8 index, u64 value) {
    ASSERT(index < 16);
#if defined(_WIN32)
    auto& r = *static_cast<EXCEPTION_POINTERS*>(ctx)->ContextRecord;
    const std::array registers{&r.Rax, &r.Rcx, &r.Rdx, &r.Rbx, &r.Rsp, &r.Rbp, &r.Rsi, &r.Rdi,
                               &r.R8,  &r.R9,  &r.R10, &r.R11, &r.R12, &r.R13, &r.R14, &r.R15};
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    auto& r = static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss;
    const std::array registers{&r.__rax, &r.__rcx, &r.__rdx, &r.__rbx, &r.__rsp, &r.__rbp,
                               &r.__rsi, &r.__rdi, &r.__r8,  &r.__r9,  &r.__r10, &r.__r11,
                               &r.__r12, &r.__r13, &r.__r14, &r.__r15};
#elif defined(__FreeBSD__)
    auto& r = static_cast<ucontext_t*>(ctx)->uc_mcontext;
    const std::array registers{&r.mc_rax, &r.mc_rcx, &r.mc_rdx, &r.mc_rbx, &r.mc_rsp, &r.mc_rbp,
                               &r.mc_rsi, &r.mc_rdi, &r.mc_r8,  &r.mc_r9,  &r.mc_r10, &r.mc_r11,
                               &r.mc_r12, &r.mc_r13, &r.mc_r14, &r.mc_r15};
#elif defined(ARCH_X86_64)
    auto& r = static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs;
    const std::array registers{&r[REG_RAX], &r[REG_RCX], &r[REG_RDX], &r[REG_RBX],
                               &r[REG_RSP], &r[REG_RBP], &r[REG_RSI], &r[REG_RDI],
                               &r[REG_R8],  &r[REG_R9],  &r[REG_R10], &r[REG_R11],
                               &r[REG_R12], &r[REG_R13], &r[REG_R14], &r[REG_R15]};
#else
    UNREACHABLE_MSG("x64 registers are unavailable on this architecture");
#endif
#ifdef ARCH_X86_64
    *registers[index] = value;
#endif
}

bool IsWriteError(void* ctx) {
#if defined(_WIN32)
    return ((EXCEPTION_POINTERS*)ctx)->ExceptionRecord->ExceptionInformation[0] == 1;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__err & 0x2;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__esr & 0x40;
#elif defined(__FreeBSD__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.mc_err & 0x2;
#elif defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_ERR] & 0x2;
#else
#error "Unsupported architecture"
#endif
}

bool IsExecuteError(void* ctx) {
#if defined(_WIN32)
    return ((EXCEPTION_POINTERS*)ctx)->ExceptionRecord->ExceptionInformation[0] == 0xf;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__err & 0x10;
#elif defined(__FreeBSD__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.mc_err & 0x10;
#elif defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_ERR] & 0x10;
#else
#error "Unsupported architecture"
#endif
}

} // namespace Common
