#include "svm_selftest.hpp"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

extern "C" {
int  cpu_number(void);
void mp_rendezvous_no_intrs(void (*action)(void *), void *arg);
}

using namespace svm;

namespace amdhv {

// Guest program (16-bit real mode, loaded at guest physical 0):
//   66 B8 78 56 34 12   mov eax, 0x12345678
//   0F A2               cpuid      -> #VMEXIT CPUID
//   F4                  hlt        -> #VMEXIT HLT
//   EB FE               jmp $      (never reached)
static const uint8_t kGuestCode[] = {0x66, 0xB8, 0x78, 0x56, 0x34, 0x12, 0x0F, 0xA2, 0xF4, 0xEB, 0xFE};
constexpr uint64_t kGuestMagic   = 0x12345678;
constexpr uint64_t kCpuidInsnLen = 2;

// Shared buffer layout (pages)
enum { P_PML4 = 0, P_PDPT, P_PD, P_PT, P_CODE, P_IOPM, P_MSRPM = P_IOPM + 3, P_SHARED_TOTAL = P_MSRPM + 2 };
// Per-CPU buffer layout (pages)
enum { P_HSAVE = 0, P_VMCB, P_PERCPU_TOTAL };

static uint64_t gHsavePa[kMaxCpus];
static uint64_t gVmcbPa[kMaxCpus];
static uint8_t *gVmcbVa[kMaxCpus];

static IOBufferMemoryDescriptor *allocContig(size_t pages, uint64_t *pa, uint8_t **va) {
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        pages * PAGE_SIZE, 0x000FFFFFFFFFF000ULL);
    if (!md) return nullptr;
    if (md->prepare() != kIOReturnSuccess) {
        md->release();
        return nullptr;
    }
    *pa = md->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    *va = static_cast<uint8_t *>(md->getBytesNoCopy());
    bzero(*va, pages * PAGE_SIZE);
    return md;
}

static void freeContig(IOBufferMemoryDescriptor *&md) {
    if (!md) return;
    md->complete();
    md->release();
    md = nullptr;
}

SvmInfo probeSvm() {
    SvmInfo info = {};
    uint32_t r[4];
    amdhv_cpuid(0x80000000, 0, r);
    if (r[0] < CPUID_SVM_INFO) return info;
    amdhv_cpuid(CPUID_EXT_FEATURES, 0, r);
    info.cpuidSvm = (r[2] >> 2) & 1;
    if (!info.cpuidSvm) return info;
    amdhv_cpuid(CPUID_SVM_INFO, 0, r);
    info.revision = r[0] & 0xFF;
    info.nasid    = r[1];
    info.features = r[3];
    return info;
}

const char *statusName(uint32_t s) {
    switch (s) {
        case kNotRun:         return "not-run";
        case kPass:           return "pass";
        case kNoSvm:          return "no-svm";
        case kSvmDisabled:    return "svm-disabled-by-bios";
        case kVmrunInvalid:   return "vmrun-invalid-vmcb";
        case kUnexpectedExit: return "unexpected-exit";
        case kWrongValue:     return "wrong-guest-value";
        case kTooManyExits:   return "too-many-exits";
        default:              return "?";
    }
}

bool SvmSelfTest::init(unsigned ncpus, bool nrips) {
    if (ncpus == 0 || ncpus > kMaxCpus) return false;
    ncpus_ = ncpus;
    nrips_ = nrips;

    uint8_t *va = nullptr;
    uint64_t pa = 0;
    shared_ = allocContig(P_SHARED_TOTAL, &pa, &va);
    if (!shared_) return false;

    auto pageVa = [&](size_t i) { return reinterpret_cast<uint64_t *>(va + i * PAGE_SIZE); };
    auto pagePa = [&](size_t i) { return pa + i * PAGE_SIZE; };

    // Nested page tables: map guest physical page 0 -> code page. Everything else faults (NPF).
    pageVa(P_PML4)[0] = pagePa(P_PDPT) | NPT_RWX;
    pageVa(P_PDPT)[0] = pagePa(P_PD)   | NPT_RWX;
    pageVa(P_PD)[0]   = pagePa(P_PT)   | NPT_RWX;
    pageVa(P_PT)[0]   = pagePa(P_CODE) | NPT_RWX;
    memcpy(pageVa(P_CODE), kGuestCode, sizeof(kGuestCode));

    // Intercept every I/O port and every MSR.
    memset(pageVa(P_IOPM), 0xFF, 3 * PAGE_SIZE);
    memset(pageVa(P_MSRPM), 0xFF, 2 * PAGE_SIZE);

    nptPa_   = pagePa(P_PML4);
    iopmPa_  = pagePa(P_IOPM);
    msrpmPa_ = pagePa(P_MSRPM);

    for (unsigned c = 0; c < ncpus_; c++) {
        uint8_t *cva = nullptr;
        uint64_t cpa = 0;
        perCpu_[c] = allocContig(P_PERCPU_TOTAL, &cpa, &cva);
        if (!perCpu_[c]) {
            release();
            return false;
        }
        gHsavePa[c] = cpa + P_HSAVE * PAGE_SIZE;
        gVmcbPa[c]  = cpa + P_VMCB * PAGE_SIZE;
        gVmcbVa[c]  = cva + P_VMCB * PAGE_SIZE;
    }
    return true;
}

void SvmSelfTest::release() {
    for (unsigned c = 0; c < kMaxCpus; c++) freeContig(perCpu_[c]);
    freeContig(shared_);
}

unsigned SvmSelfTest::passCount() const {
    unsigned n = 0;
    for (unsigned c = 0; c < ncpus_; c++) n += (res_[c].status == kPass);
    return n;
}

void SvmSelfTest::setupVmcb(uint8_t *vmcb) {
    bzero(vmcb, PAGE_SIZE);

    // ---- control area ----
    vmcb_set<uint32_t>(vmcb, VMCB_INTERCEPT_EXC, 0xFFFFFFFF);    // every exception exits
    vmcb_set<uint32_t>(vmcb, VMCB_INTERCEPT_MISC1,
                       ICPT_NMI | ICPT_SMI | ICPT_INIT | ICPT_CPUID | ICPT_HLT |
                       ICPT_INVLPGA | ICPT_IOIO | ICPT_MSR | ICPT_SHUTDOWN);
    vmcb_set<uint32_t>(vmcb, VMCB_INTERCEPT_MISC2,
                       ICPT_VMRUN | ICPT_VMMCALL | ICPT_VMLOAD | ICPT_VMSAVE |
                       ICPT_STGI | ICPT_CLGI | ICPT_SKINIT | ICPT_XSETBV);
    vmcb_set<uint64_t>(vmcb, VMCB_IOPM_BASE, iopmPa_);
    vmcb_set<uint64_t>(vmcb, VMCB_MSRPM_BASE, msrpmPa_);
    vmcb_set<uint32_t>(vmcb, VMCB_ASID, 1);
    vmcb_set<uint8_t>(vmcb, VMCB_TLB_CONTROL, 1);                // flush everything
    vmcb_set<uint64_t>(vmcb, VMCB_VINTR, VINTR_MASKING);         // physical IRQs stay with host
    vmcb_set<uint64_t>(vmcb, VMCB_NP_ENABLE, 1);
    vmcb_set<uint64_t>(vmcb, VMCB_NCR3, nptPa_);
    vmcb_set<uint32_t>(vmcb, VMCB_CLEAN_BITS, 0);

    // ---- guest state: 16-bit real mode, CS:IP = 0000:0000 ----
    vmcb_set_seg(vmcb, VMCB_CS, 0, 0x009B, 0xFFFF, 0);
    vmcb_set_seg(vmcb, VMCB_DS, 0, 0x0093, 0xFFFF, 0);
    vmcb_set_seg(vmcb, VMCB_ES, 0, 0x0093, 0xFFFF, 0);
    vmcb_set_seg(vmcb, VMCB_SS, 0, 0x0093, 0xFFFF, 0);
    vmcb_set_seg(vmcb, VMCB_FS, 0, 0x0093, 0xFFFF, 0);
    vmcb_set_seg(vmcb, VMCB_GS, 0, 0x0093, 0xFFFF, 0);
    vmcb_set_seg(vmcb, VMCB_GDTR, 0, 0, 0xFFFF, 0);
    vmcb_set_seg(vmcb, VMCB_IDTR, 0, 0, 0x3FF, 0);
    vmcb_set_seg(vmcb, VMCB_LDTR, 0, 0x0082, 0xFFFF, 0);
    vmcb_set_seg(vmcb, VMCB_TR,   0, 0x008B, 0xFFFF, 0);
    vmcb_set<uint8_t>(vmcb, VMCB_CPL, 0);
    vmcb_set<uint64_t>(vmcb, VMCB_EFER, EFER_SVME);              // required by VMRUN checks
    vmcb_set<uint64_t>(vmcb, VMCB_CR0, 0x10);                    // ET only: PE=0, PG=0
    vmcb_set<uint64_t>(vmcb, VMCB_CR3, 0);
    vmcb_set<uint64_t>(vmcb, VMCB_CR4, 0);
    vmcb_set<uint64_t>(vmcb, VMCB_DR6, 0xFFFF0FF0);
    vmcb_set<uint64_t>(vmcb, VMCB_DR7, 0x400);
    vmcb_set<uint64_t>(vmcb, VMCB_RFLAGS, 0x2);                  // IF = 0
    vmcb_set<uint64_t>(vmcb, VMCB_RIP, 0);
    vmcb_set<uint64_t>(vmcb, VMCB_RSP, 0xFFF0);
    vmcb_set<uint64_t>(vmcb, VMCB_RAX, 0);
    vmcb_set<uint64_t>(vmcb, VMCB_G_PAT, 0x0007040600070406ULL);
}

// Runs with interrupts disabled on every CPU (mp_rendezvous_no_intrs).
void SvmSelfTest::onCpu(void *arg) {
    auto *self = static_cast<SvmSelfTest *>(arg);
    int cpu = cpu_number();
    if (cpu < 0 || (unsigned)cpu >= self->ncpus_) return;
    self->runThisCpu((unsigned)cpu);
}

void SvmSelfTest::runThisCpu(unsigned cpu) {
    CpuResult &r = res_[cpu];

    uint32_t id[4];
    amdhv_cpuid(CPUID_EXT_FEATURES, 0, id);
    if (!((id[2] >> 2) & 1)) {
        r.status = kNoSvm;
        return;
    }
    r.vmcr = amdhv_rdmsr(MSR_VM_CR);
    if (r.vmcr & VM_CR_SVMDIS) {
        r.status = kSvmDisabled;
        return;
    }

    const uint64_t efer  = amdhv_rdmsr(MSR_EFER);
    const uint64_t hsave = amdhv_rdmsr(MSR_VM_HSAVE_PA);
    const uint64_t dr7   = amdhv_read_dr7();
    r.eferBefore = efer;

    amdhv_wrmsr(MSR_EFER, efer | EFER_SVME);
    amdhv_wrmsr(MSR_VM_HSAVE_PA, gHsavePa[cpu]);

    uint8_t *vmcb = gVmcbVa[cpu];
    setupVmcb(vmcb);
    GuestGprs gprs = {};
    bool sawCpuid = false;

    r.status = kTooManyExits;
    for (unsigned i = 0; i < 8; i++) {
        amdhv_svm_vmrun(gVmcbPa[cpu], &gprs);
        vmcb_set<uint32_t>(vmcb, VMCB_CLEAN_BITS, 0);
        vmcb_set<uint8_t>(vmcb, VMCB_TLB_CONTROL, 0);

        const uint64_t code = vmcb_get<uint64_t>(vmcb, VMCB_EXITCODE);
        r.exitCodes[r.exits++] = code;
        r.exitInfo1 = vmcb_get<uint64_t>(vmcb, VMCB_EXITINFO1);

        if (code == EXIT_INVALID) { r.status = kVmrunInvalid; break; }
        if (code == EXIT_NMI || code == EXIT_SMI || code == EXIT_INTR) continue; // host handles after STGI
        if (code == EXIT_CPUID) {
            r.guestRaxAtCpuid = vmcb_get<uint64_t>(vmcb, VMCB_RAX);
            if ((r.guestRaxAtCpuid & 0xFFFFFFFF) != kGuestMagic) { r.status = kWrongValue; break; }
            sawCpuid = true;
            uint64_t next = nrips_ ? vmcb_get<uint64_t>(vmcb, VMCB_NRIP)
                                   : vmcb_get<uint64_t>(vmcb, VMCB_RIP) + kCpuidInsnLen;
            vmcb_set<uint64_t>(vmcb, VMCB_RIP, next);
            continue;
        }
        if (code == EXIT_HLT && sawCpuid) { r.status = kPass; break; }
        r.status = kUnexpectedExit;
        break;
    }

    amdhv_write_dr7(dr7);
    amdhv_wrmsr(MSR_VM_HSAVE_PA, hsave);
    amdhv_wrmsr(MSR_EFER, efer);
    r.eferAfter = amdhv_rdmsr(MSR_EFER);
}

void SvmSelfTest::run() {
    bzero(res_, sizeof(res_));
    mp_rendezvous_no_intrs(&SvmSelfTest::onCpu, this);
}

} // namespace amdhv
