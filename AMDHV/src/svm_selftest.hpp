#pragma once
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "svm_defs.hpp"

namespace amdhv {

constexpr unsigned kMaxCpus = 128;

struct SvmInfo {
    bool     cpuidSvm;      // CPUID 0x80000001 ECX[2]
    uint32_t revision;      // CPUID 0x8000000A EAX[7:0]
    uint32_t nasid;         // number of ASIDs
    uint32_t features;      // CPUID 0x8000000A EDX
};

enum TestStatus : uint32_t {
    kNotRun = 0,
    kPass,
    kNoSvm,          // CPU does not report SVM
    kSvmDisabled,    // VM_CR.SVMDIS set (BIOS)
    kVmrunInvalid,   // VMRUN rejected the VMCB (exit code -1)
    kUnexpectedExit,
    kWrongValue,     // CPUID exit seen but guest RAX is wrong
    kTooManyExits,
};

struct CpuResult {
    uint32_t status;
    uint32_t exits;
    uint64_t exitCodes[8];
    uint64_t exitInfo1;     // of the last exit
    uint64_t guestRaxAtCpuid;
    uint64_t vmcr;
    uint64_t eferBefore;
    uint64_t eferAfter;
};

SvmInfo probeSvm();
const char *statusName(uint32_t s);

// Runs a tiny real-mode guest on every CPU:
//   mov eax, 0x12345678 ; cpuid ; hlt
// Expected exits: CPUID (with guest RAX = 0x12345678), then HLT.
// SVM is turned off again and EFER/VM_HSAVE_PA restored on every CPU afterwards.
class SvmSelfTest {
public:
    bool init(unsigned ncpus, bool nrips);
    void run();
    void release();

    unsigned  cpuCount() const { return ncpus_; }
    const CpuResult &result(unsigned cpu) const { return res_[cpu]; }
    unsigned  passCount() const;

private:
    static void onCpu(void *arg);
    void runThisCpu(unsigned cpu);
    void setupVmcb(uint8_t *vmcb);

    unsigned ncpus_ = 0;
    bool nrips_ = false;

    IOBufferMemoryDescriptor *shared_ = nullptr;     // NPT (4 pages) + code + IOPM (3) + MSRPM (2)
    IOBufferMemoryDescriptor *perCpu_[kMaxCpus] = {}; // host save area + VMCB
    uint64_t nptPa_ = 0, iopmPa_ = 0, msrpmPa_ = 0;
    CpuResult res_[kMaxCpus] = {};
};

} // namespace amdhv
