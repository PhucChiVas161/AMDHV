#pragma once
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "vcpu_state.hpp"
#include "vcpu_interrupt.hpp"
namespace amdhv {
class GuestMemory;
bool fastRunEnabled();
void setFastRunEnabled(bool enabled); // only with takeover disabled
bool fullExitTraceEnabled();
void setFullExitTraceEnabled(bool enabled); // runtime diagnostic, no execution-policy change
void logVcpuState(const VcpuState &state);
// Raw host CPUID facts used on every entry. All cores of a supported package
// report the same leaves, so they are read once before traps are installed
// instead of executing ~15 serializing CPUIDs per run call.
void hostCapsInit();
// Sleep/wake (or any event that may have discarded hardware TLB state without
// this backend observing it) forces one ASID flush per vCPU on its next entry.
void bumpFlushEpoch();
class VcpuSvm {
    IOBufferMemoryDescriptor *memory_ = nullptr;
    uint8_t *bytes_ = nullptr;
    uint64_t physical_ = 0;
    uint64_t successfulExits_ = 0, tracedExits_ = 0;
    uint64_t runs_ = 0, tlbFlushes_ = 0, forwardedMsrs_ = 0;
    bool hostOnlyExit_ = false;
    // Guest TLB ownership. A vCPU with a private ASID keeps its translations
    // across run calls and flushes only when they may be stale: first entry,
    // a different physical CPU, an NPT edit, a paging-register change made
    // outside the guest, an emulated CR/INVLPG, or an explicit VMM request.
    // Without a private ASID every run call flushes, as before 0.3.41.
    uint32_t asid_ = 1;
    bool privateAsid_ = false, needFlush_ = true;
    int lastCpu_ = -1;
    uint64_t seenGeneration_ = 0, seenEpoch_ = 0;
    uint64_t lastCr0_ = 0, lastCr3_ = 0, lastCr4_ = 0, lastEfer_ = 0;
public:
    bool init();
    void release();
    // `index` is unique among live vCPUs of all VMs (slot * MaxVcpus + id).
    void assignAsid(unsigned index);
    void requestFlush() { needFlush_ = true; }
    // True when the last run() ended only because of the host (physical IRQ/NMI
    // or the in-kernel exit budget) with nothing for the VMM to act on: no
    // cancellation and no interrupted event delivery. Such a quantum can be
    // continued by calling run() again without going back to user space.
    bool hostOnlyExit() const { return hostOnlyExit_; }
    // Fast code64 switches a CPUID-validated user-state subset including
    // x87/SSE/AVX/AVX512/PKRU; supervisor and larger/unknown layouts stay gated.
    kern_return_t run(VcpuState &state, GuestMemory &memory, VcpuInterrupts &interrupts, unsigned cpuId);
};
}
