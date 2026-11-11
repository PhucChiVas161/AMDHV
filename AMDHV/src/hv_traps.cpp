#include "hv_traps.hpp"
#include "klog.hpp"
#include "vm_backend.hpp"
#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#include <libkern/libkern.h>
#include <sys/proc.h>


namespace amdhv {

// Names follow AppleHV 25G332 (research/AppleHV-25G332.s, task_traps / thread_traps).
static const char *const kTaskNames[kTaskTrapCount] = {
    "vm_create", "vm_destroy", "vm_addrspace_create", "vm_addrspace_destroy",
    "vm_map", "vm_unmap", "vm_protect", "vm_sync_tsc",
    "vm_add_io_notifier", "vm_rem_io_notifier", "vm_request", "vm_intr_msi",
    "vcpu_create", "vcpu_interrupt",
};
static const char *const kThreadNames[kThreadTrapCount] = {
    "vcpu_run", "vcpu_run_until", "vcpu_invalidate_tlb", "vcpu_enable_managed_msr",
    "vcpu_disable_managed_msr", "vcpu_destroy", "vcpu_set_addrspace", "vmx_vcpu_read_vmcs",
    "vmx_vcpu_set_shadow_access", "vmx_vcpu_set_apic_address", "vmx_vcpu_set_msr_access",
    "vmx_vcpu_read_drs", "vmx_vcpu_get_cap_write_vmcs", "vmx_vcpu_request",
};

static volatile SInt32 gCounts[2][14];
static volatile SInt32 gLogged = 0;
constexpr SInt32 kMaxLogged = 400;

const char *TrapHost::trapName(int type, unsigned idx) {
    if (idx >= 14) return "?";
    return type == HV_TASK_TRAP ? kTaskNames[idx] : kThreadNames[idx];
}

uint32_t TrapHost::count(int type, unsigned idx) const {
    return idx < 14 ? (uint32_t)gCounts[type & 1][idx] : 0;
}

static bool looksLikeUserPtr(uint64_t v) {
    return v >= 0x1000 && v < 0x0000800000000000ULL;
}

static kern_return_t onTrap(int type, unsigned idx, void *target, uint64_t arg) {
    OSIncrementAtomic(&gCounts[type][idx]);
    const bool logged = OSIncrementAtomic(&gLogged) < kMaxLogged;
    if (logged) {
        char pname[32] = {};
        proc_selfname(pname, sizeof(pname));
        klog("%s[%u] %s pid=%d (%s) target=%p arg=0x%llx",
             type == HV_TASK_TRAP ? "task" : "thread", idx, TrapHost::trapName(type, idx),
             proc_selfpid(), pname, target, arg);
        uint64_t q[8];
        if (looksLikeUserPtr(arg) && copyin((user_addr_t)arg, q, sizeof(q)) == 0) {
            klog("    *arg: %016llx %016llx %016llx %016llx", q[0], q[1], q[2], q[3]);
            klog("          %016llx %016llx %016llx %016llx", q[4], q[5], q[6], q[7]);
        }
    }
    kern_return_t result = vmBackendTrap(type, idx, arg);
    if (logged) klog("%s[%u] %s pid=%d result=0x%x",
        type == HV_TASK_TRAP ? "task" : "thread", idx, TrapHost::trapName(type, idx),
        proc_selfpid(), (unsigned)result);
    return result;
}

template <int T, unsigned I>
static kern_return_t stubTrap(void *target, uint64_t arg) {
    return onTrap(T, I, target, arg);
}

#define STUBS(T) \
    { &stubTrap<T, 0>, &stubTrap<T, 1>, &stubTrap<T, 2>,  &stubTrap<T, 3>,  &stubTrap<T, 4>, \
      &stubTrap<T, 5>, &stubTrap<T, 6>, &stubTrap<T, 7>,  &stubTrap<T, 8>,  &stubTrap<T, 9>, \
      &stubTrap<T, 10>, &stubTrap<T, 11>, &stubTrap<T, 12>, &stubTrap<T, 13> }

static const hv_trap_t gTaskTraps[kTaskTrapCount]     = STUBS(HV_TASK_TRAP);
static const hv_trap_t gThreadTraps[kThreadTrapCount] = STUBS(HV_THREAD_TRAP);

bool TrapHost::takeover(char *why, unsigned whyLen) {
    if (active_) {
        snprintf(why, whyLen, "already active");
        return true;
    }
    if (!resolved_) {
        uint64_t slide = 0;
        if (!resolveKernelApi(&api_, why, whyLen, &slide)) {
            klog("takeover: kernel API not resolved: %s", why);
            return false;
        }
        resolved_ = true;
        klog("takeover: kernel API resolved, slide=0x%llx, hv_support=%d",
             slide, *api_.hv_support_available);
    }

    if (!vmBackendInit(api_)) {
        snprintf(why, whyLen, "VM backend allocation failed");
        return false;
    }
    runtimePinned_ = true; // Published raw callbacks require reboot before kext removal.
    // AppleHV registered its traps at boot even though VMX is unavailable on AMD.
    api_.release_traps(HV_TASK_TRAP);
    api_.release_traps(HV_THREAD_TRAP);
    api_.release_callbacks();
    if (api_.set_callbacks(vmBackendCallbacks()) != KERN_SUCCESS) {
        snprintf(why, whyLen, "hv_set_callbacks failed");
        return false;
    }

    kern_return_t kr1 = api_.set_traps(HV_TASK_TRAP, gTaskTraps, kTaskTrapCount);
    kern_return_t kr2 = api_.set_traps(HV_THREAD_TRAP, gThreadTraps, kThreadTrapCount);
    if (kr1 != KERN_SUCCESS || kr2 != KERN_SUCCESS) {
        api_.release_traps(HV_TASK_TRAP);
        api_.release_traps(HV_THREAD_TRAP);
        api_.release_callbacks();
        snprintf(why, whyLen, "hv_set_traps failed (%d, %d)", kr1, kr2);
        klog("takeover: %s", why);
        return false;
    }

    vmBackendEnable();
    savedSupport_ = *api_.hv_support_available;
    *api_.hv_support_available = 1;
    active_ = true;
    gLogged = 0;
    snprintf(why, whyLen, "ok");
    klog("takeover: traps installed (%u task, %u thread), kern.hv_support=1",
         kTaskTrapCount, kThreadTrapCount);
    return true;
}

bool TrapHost::restore() {
    if (!active_) return true;
    if (!vmBackendQuiesce()) {
        klog("restore: refused while %u VMs are alive", vmBackendCount());
        return false;
    }
    *api_.hv_support_available = savedSupport_;
    api_.release_traps(HV_TASK_TRAP);
    api_.release_traps(HV_THREAD_TRAP);
    api_.release_callbacks();
    active_ = false;
    klog("restore: traps released, kern.hv_support=%d", savedSupport_);
    return true;
}

} // namespace amdhv
