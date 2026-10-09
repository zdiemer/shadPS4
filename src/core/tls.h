// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>
#include <tuple>
#include <type_traits>
#include "common/types.h"
#ifdef _WIN32
#include <malloc.h>
#endif

namespace Xbyak {
class CodeGenerator;
}

namespace Libraries::Fiber {
struct OrbisFiberContext;
}

namespace Core {

union DtvEntry {
    std::size_t counter;
    u8* pointer;
};

struct Tcb {
    Tcb* tcb_self;
    DtvEntry* tcb_dtv;
    void* tcb_thread;
    void* tcb_spare[2];
    u64 tcb_canary;
    ::Libraries::Fiber::OrbisFiberContext* tcb_fiber;
};

#ifdef _WIN32
/// Gets the thread local storage key for the TCB block.
u32 GetTcbKey();
#endif

/// Sets the data pointer to the TCB block.
void SetTcbBase(void* image_address);

/// Retrieves Tcb structure for the calling thread.
Tcb* GetTcbBase();

/// Makes sure TLS is initialized for the thread before entering guest.
void InitializeTLS();

extern thread_local void* g_fiber_hle_stack;
void* PS4_SYSV_ABI GetHleStack();
void* PS4_SYSV_ABI RunOnHleStack(void* PS4_SYSV_ABI (*func)(void*), void* arg, void* stack);

template <auto f>
struct HostCallWrapperImpl;

template <class ReturnType, class... Args, PS4_SYSV_ABI ReturnType (*func)(Args...)>
struct HostCallWrapperImpl<func> {
    [[gnu::noinline]] static ReturnType PS4_SYSV_ABI Call(Args... args) {
        return func(args...);
    }

    struct CallContext {
        std::tuple<Args...> args;
        std::conditional_t<std::is_void_v<ReturnType>, u8, ReturnType> result{};
    };

    static void* PS4_SYSV_ABI Invoke(void* arg) {
        auto& context = *static_cast<CallContext*>(arg);
        if constexpr (std::is_void_v<ReturnType>) {
            std::apply(Call, context.args);
        } else {
            context.result = std::apply(Call, context.args);
        }
        return nullptr;
    }

    static ReturnType PS4_SYSV_ABI wrap(Args... args) {
        if (void* stack = GetHleStack()) {
            CallContext context{{args...}};
            RunOnHleStack(Invoke, &context, stack);
            if constexpr (!std::is_void_v<ReturnType>) {
                return context.result;
            }
        } else {
            return Call(args...);
        }
    }
};

#define HOST_CALL(func) (Core::HostCallWrapperImpl<func>::wrap)

} // namespace Core
