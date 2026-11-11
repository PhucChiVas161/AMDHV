// AMD SVM definitions (AMD64 APM Vol. 2, chapter 15 and appendix B).
#pragma once
#include <stdint.h>

namespace svm {

// ---- MSRs ----
constexpr uint32_t MSR_EFER        = 0xC0000080;
constexpr uint32_t MSR_VM_CR       = 0xC0010114;
constexpr uint32_t MSR_VM_HSAVE_PA = 0xC0010117;

constexpr uint64_t EFER_SVME    = 1ull << 12;
constexpr uint64_t VM_CR_LOCK   = 1ull << 3;
constexpr uint64_t VM_CR_SVMDIS = 1ull << 4;

// ---- CPUID ----
constexpr uint32_t CPUID_EXT_FEATURES = 0x80000001; // ECX bit 2 = SVM
constexpr uint32_t CPUID_SVM_INFO     = 0x8000000A;  // EAX rev, EBX nASID, EDX features

// CPUID 0x8000000A EDX feature bits
enum : uint32_t {
    FEAT_NP          = 1u << 0,
    FEAT_LBRV        = 1u << 1,
    FEAT_SVML        = 1u << 2,
    FEAT_NRIPS       = 1u << 3,
    FEAT_TSC_RATE    = 1u << 4,
    FEAT_VMCB_CLEAN  = 1u << 5,
    FEAT_FLUSH_ASID  = 1u << 6,
    FEAT_DECODE      = 1u << 7,
    FEAT_PAUSE_FILT  = 1u << 10,
    FEAT_PAUSE_THR   = 1u << 12,
    FEAT_AVIC        = 1u << 13,
    FEAT_VLS         = 1u << 15,
    FEAT_VGIF        = 1u << 16,
    FEAT_GMET        = 1u << 17,
    FEAT_X2AVIC      = 1u << 18,
    FEAT_VNMI        = 1u << 25,
};

// ---- VMCB control area offsets ----
constexpr uint32_t VMCB_INTERCEPT_CR    = 0x000;
constexpr uint32_t VMCB_INTERCEPT_DR    = 0x004;
constexpr uint32_t VMCB_INTERCEPT_EXC   = 0x008;
constexpr uint32_t VMCB_INTERCEPT_MISC1 = 0x00C;
constexpr uint32_t VMCB_INTERCEPT_MISC2 = 0x010;
constexpr uint32_t VMCB_IOPM_BASE       = 0x040;
constexpr uint32_t VMCB_MSRPM_BASE      = 0x048;
constexpr uint32_t VMCB_TSC_OFFSET      = 0x050;
constexpr uint32_t VMCB_ASID            = 0x058;
constexpr uint32_t VMCB_TLB_CONTROL     = 0x05C;
constexpr uint32_t VMCB_VINTR           = 0x060;
constexpr uint32_t VMCB_INT_SHADOW      = 0x068;
constexpr uint32_t VMCB_EXITCODE        = 0x070;
constexpr uint32_t VMCB_EXITINFO1       = 0x078;
constexpr uint32_t VMCB_EXITINFO2       = 0x080;
constexpr uint32_t VMCB_EXITINTINFO     = 0x088;
constexpr uint32_t VMCB_NP_ENABLE       = 0x090;
constexpr uint32_t VMCB_EVENTINJ        = 0x0A8;
constexpr uint32_t VMCB_NCR3            = 0x0B0;
constexpr uint32_t VMCB_CLEAN_BITS      = 0x0C0;
constexpr uint32_t VMCB_NRIP            = 0x0C8;

// ---- VMCB state save area offsets (start at 0x400) ----
constexpr uint32_t VMCB_ES     = 0x400;  // each segment: sel u16, attr u16, limit u32, base u64
constexpr uint32_t VMCB_CS     = 0x410;
constexpr uint32_t VMCB_SS     = 0x420;
constexpr uint32_t VMCB_DS     = 0x430;
constexpr uint32_t VMCB_FS     = 0x440;
constexpr uint32_t VMCB_GS     = 0x450;
constexpr uint32_t VMCB_GDTR   = 0x460;
constexpr uint32_t VMCB_LDTR   = 0x470;
constexpr uint32_t VMCB_IDTR   = 0x480;
constexpr uint32_t VMCB_TR     = 0x490;
constexpr uint32_t VMCB_CPL    = 0x4CB;
constexpr uint32_t VMCB_EFER   = 0x4D0;
constexpr uint32_t VMCB_CR4    = 0x548;
constexpr uint32_t VMCB_CR3    = 0x550;
constexpr uint32_t VMCB_CR0    = 0x558;
constexpr uint32_t VMCB_DR7    = 0x560;
constexpr uint32_t VMCB_DR6    = 0x568;
constexpr uint32_t VMCB_RFLAGS = 0x570;
constexpr uint32_t VMCB_RIP    = 0x578;
constexpr uint32_t VMCB_RSP    = 0x5D8;
constexpr uint32_t VMCB_RAX    = 0x5F8;
constexpr uint32_t VMCB_CR2    = 0x640;
constexpr uint32_t VMCB_G_PAT  = 0x668;
constexpr uint32_t VMCB_SPEC_CTRL = 0x6E0; // save-area0x2e0; CPUID8000000a.EDX[20]

// Intercept vector 3 (offset 0x00C)
enum : uint32_t {
    ICPT_INTR     = 1u << 0,
    ICPT_NMI      = 1u << 1,
    ICPT_SMI      = 1u << 2,
    ICPT_INIT     = 1u << 3,
    ICPT_CPUID    = 1u << 18,
    ICPT_HLT      = 1u << 24,
    ICPT_INVLPGA  = 1u << 26,
    ICPT_IOIO     = 1u << 27,
    ICPT_MSR      = 1u << 28,
    ICPT_SHUTDOWN = 1u << 31,
};
// Intercept vector 4 (offset 0x010)
enum : uint32_t {
    ICPT_VMRUN   = 1u << 0,   // must always be set, otherwise VMRUN fails
    ICPT_VMMCALL = 1u << 1,
    ICPT_VMLOAD  = 1u << 2,
    ICPT_VMSAVE  = 1u << 3,
    ICPT_STGI    = 1u << 4,
    ICPT_CLGI    = 1u << 5,
    ICPT_SKINIT  = 1u << 6,
    ICPT_XSETBV  = 1u << 13,
};

constexpr uint64_t VINTR_MASKING = 1ull << 24;  // host RFLAGS.IF masks physical interrupts

// Exit codes
constexpr uint64_t EXIT_EXCP_BASE = 0x40;
constexpr uint64_t EXIT_INTR      = 0x60;
constexpr uint64_t EXIT_NMI       = 0x61;
constexpr uint64_t EXIT_SMI       = 0x62;
constexpr uint64_t EXIT_INIT      = 0x63;
constexpr uint64_t EXIT_CPUID     = 0x72;
constexpr uint64_t EXIT_HLT       = 0x78;
constexpr uint64_t EXIT_SHUTDOWN  = 0x7F;
constexpr uint64_t EXIT_NPF       = 0x400;
constexpr uint64_t EXIT_INVALID   = ~0ull;

// Nested page table entry bits (same format as normal x86-64 paging; U must be set)
constexpr uint64_t NPT_P  = 1ull << 0;
constexpr uint64_t NPT_RW = 1ull << 1;
constexpr uint64_t NPT_US = 1ull << 2;
constexpr uint64_t NPT_RWX = NPT_P | NPT_RW | NPT_US;

// Guest general purpose registers not held in the VMCB (RAX/RSP live in the VMCB).
struct GuestGprs {
    uint64_t rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
};

} // namespace svm

// ---- Low level helpers ----
static inline uint64_t amdhv_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void amdhv_wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}
static inline void amdhv_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}
static inline uint64_t amdhv_read_dr7() {
    uint64_t v; __asm__ volatile("mov %%dr7, %0" : "=r"(v)); return v;
}
static inline void amdhv_write_dr7(uint64_t v) {
    __asm__ volatile("mov %0, %%dr7" :: "r"(v));
}

// VMCB field accessors
template <typename T> static inline T vmcb_get(const uint8_t *vmcb, uint32_t off) {
    return *reinterpret_cast<const volatile T *>(vmcb + off);
}
template <typename T> static inline void vmcb_set(uint8_t *vmcb, uint32_t off, T v) {
    *reinterpret_cast<volatile T *>(vmcb + off) = v;
}
static inline void vmcb_set_seg(uint8_t *vmcb, uint32_t off, uint16_t sel, uint16_t attr,
                                uint32_t limit, uint64_t base) {
    vmcb_set<uint16_t>(vmcb, off + 0, sel);
    vmcb_set<uint16_t>(vmcb, off + 2, attr);
    vmcb_set<uint32_t>(vmcb, off + 4, limit);
    vmcb_set<uint64_t>(vmcb, off + 8, base);
}

// Implemented in svm_vmrun.S. Runs the guest once; returns after #VMEXIT with GIF set again.
extern "C" void amdhv_svm_vmrun(uint64_t vmcb_pa, svm::GuestGprs *gprs);

static inline uint64_t amdhv_read_tsc() {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
}
