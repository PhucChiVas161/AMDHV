#include "kernel_resolver.hpp"
#include "kernsyms.h"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <sys/sysctl.h>

extern "C" {
extern const char version[];        // Libkern KPI
}

namespace amdhv {

// kern.hv_support is SYSCTL_INT(_kern, ..., &hv_support_available): oid_arg1 is the variable.
static int *findHvSupportSysctl() {
    struct sysctl_oid *oid;
    SLIST_FOREACH(oid, &sysctl__kern_children, oid_link) {
        if (oid->oid_name && strcmp(oid->oid_name, "hv_support") == 0 &&
            (oid->oid_kind & CTLTYPE) == CTLTYPE_INT) {
            return static_cast<int *>(oid->oid_arg1);
        }
    }
    return nullptr;
}

bool resolveKernelApi(KernelApi *api, char *why, unsigned whyLen, uint64_t *slideOut) {
    using namespace kernsyms;

    // 1. Same kernel build as the matching KDK + Boot KC used for the table.
    if (strncmp(version, kVersion, sizeof(kVersion) - 1) != 0) {
        snprintf(why, whyLen, "kernel version differs from KDK table");
        return false;
    }

    // 2. Text slide from an exported function. Only used for kernel code: in the boot
    //    kernel collection the data segments do not move by the same amount.
    const uint64_t slide = (uint64_t)(uintptr_t)&IOLog - kAnchorFunc;
    *slideOut = slide;

    // 3. Every function must start with the exact bytes from the linked Boot KC.
    void *fn[F_COUNT];
    for (unsigned i = 0; i < F_COUNT; i++) {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(kFuncs[i].unslid + slide);
        if (memcmp(p, kFuncs[i].sig, kSigLen) != 0) {
            unsigned offset = 0;
            while (offset < kSigLen && p[offset] == kFuncs[i].sig[offset]) ++offset;
            snprintf(why, whyLen, "code signature mismatch: %s +%u (%02x != %02x)",
                     kFuncs[i].name, offset, p[offset], kFuncs[i].sig[offset]);
            return false;
        }
        fn[i] = const_cast<uint8_t *>(p);
    }

    // 4. hv_support_available comes from the exported sysctl tree (kern.hv_support).
    int *sup = findHvSupportSysctl();
    if (!sup) {
        snprintf(why, whyLen, "sysctl kern.hv_support not found");
        return false;
    }

    api->set_traps         = reinterpret_cast<decltype(api->set_traps)>(fn[F_hv_set_traps]);
    api->release_traps     = reinterpret_cast<decltype(api->release_traps)>(fn[F_hv_release_traps]);
    api->set_callbacks     = reinterpret_cast<decltype(api->set_callbacks)>(fn[F_hv_set_callbacks]);
    api->release_callbacks = reinterpret_cast<decltype(api->release_callbacks)>(fn[F_hv_release_callbacks]);
    api->set_task_target   = reinterpret_cast<decltype(api->set_task_target)>(fn[F_hv_set_task_target]);
    api->set_thread_target = reinterpret_cast<decltype(api->set_thread_target)>(fn[F_hv_set_thread_target]);
    api->get_task_target   = reinterpret_cast<decltype(api->get_task_target)>(fn[F_hv_get_task_target]);
    api->get_thread_target = reinterpret_cast<decltype(api->get_thread_target)>(fn[F_hv_get_thread_target]);
    api->cpus_kick         = reinterpret_cast<decltype(api->cpus_kick)>(fn[F_mp_cpus_kick]);
    api->ast_pending       = reinterpret_cast<decltype(api->ast_pending)>(fn[F_hv_ast_pending]);
    api->hv_support_available = sup;
    snprintf(why, whyLen, "ok");
    return true;
}

} // namespace amdhv
