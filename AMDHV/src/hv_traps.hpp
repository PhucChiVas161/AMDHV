// Trap takeover and dispatch to the AMD VM backend.
#pragma once
#include <stdint.h>
#include "kernel_resolver.hpp"

namespace amdhv {

constexpr unsigned kTaskTrapCount   = 14;   // same as AppleHV 25G332
constexpr unsigned kThreadTrapCount = 14;

// Hypervisor.framework return codes (Hypervisor/hv_error.h)
constexpr kern_return_t HV_SUCCESS     = 0;
constexpr kern_return_t HV_ERROR       = (kern_return_t)0xfae94001;
constexpr kern_return_t HV_UNSUPPORTED = (kern_return_t)0xfae9400f;

class TrapHost {
public:
    // Releases AppleHV's traps, installs ours, sets kern.hv_support = 1.
    bool takeover(char *why, unsigned whyLen);
    // Removes our traps and sets kern.hv_support back. AppleHV stays inactive until reboot.
    bool restore(); // false while VM targets remain; caller must not unload

    bool active() const { return active_; }
    bool runtimePinned() const { return runtimePinned_; }
    uint32_t count(int type, unsigned idx) const;
    static const char *trapName(int type, unsigned idx);

private:
    KernelApi api_ = {};
    bool resolved_ = false;
    bool active_ = false;
    bool runtimePinned_ = false;
    int  savedSupport_ = 0;
};

} // namespace amdhv
