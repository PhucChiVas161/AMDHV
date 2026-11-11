// Locates private XNU hv_* functions that third-party kexts cannot link against.
// Everything is verified (kernel version, code signature bytes of every function,
// hv_support pointer taken from the exported sysctl tree) before anything is used.
#pragma once
#include <stdint.h>
#include <mach/kern_return.h>

namespace amdhv {

// XNU osfmk/kern/hv_support_kext.h
enum HvTrapType : int { HV_TASK_TRAP = 0, HV_THREAD_TRAP = 1 };
typedef kern_return_t (*hv_trap_t)(void *target, uint64_t arg);

struct HvCallbacks {               // 8 pointers, passed by value (checked against AppleHV)
    void (*dispatch)(void *vcpu);
    void (*preempt)(void *vcpu);
    void (*suspend)(void);
    void (*thread_destroy)(void *vcpu);
    void (*task_destroy)(void *vm);
    void (*volatile_state)(void *vcpu, int state);
    void (*resume)(void);
    void (*memory_pressure)(void);
};

struct KernelApi {
    kern_return_t (*set_traps)(HvTrapType type, const hv_trap_t *traps, unsigned count);
    void          (*release_traps)(HvTrapType type);
    kern_return_t (*set_callbacks)(HvCallbacks callbacks);
    void          (*release_callbacks)(void);
    void          (*set_task_target)(void *target);
    void          (*set_thread_target)(void *target);
    void *        (*get_task_target)(void);
    void *        (*get_thread_target)(void);
    // osfmk/i386/mp.c: sends the no-op "kick" IPI to every CPU in the mask
    // (never the caller's own). AppleHV uses it to force a VM exit.
    void          (*cpus_kick)(uint64_t cpus);
    // Nonzero when the current CPU has an AST pending for the running thread
    // (signal, suspend, quantum expiry): it can only be taken at return to user.
    int           (*ast_pending)(void);
    int           *hv_support_available;   // backs sysctl kern.hv_support
};

// Returns true and fills api only if every check passes. On failure, `why` explains.
bool resolveKernelApi(KernelApi *api, char *why, unsigned whyLen, uint64_t *slideOut);

} // namespace amdhv
