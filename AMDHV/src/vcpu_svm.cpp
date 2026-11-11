#include "vcpu_svm.hpp"
#include "svm_defs.hpp"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>
#include "klog.hpp"
#include "vcpu_mode.hpp"
#include "msr_emulation.hpp"
#include "cr_emulation.hpp"
#include "guest_memory.hpp"
#include "guest_fetch.hpp"
#include "io_exit.hpp"
#include "speculation_policy.hpp"
// svm_vmrun.S: one FP/XCR0 switch per quantum, one lean entry per VMRUN.
extern "C" void amdhv_svm_fp_enter(void *guestFp, void *hostXsave);
extern "C" void amdhv_svm_fp_leave(void *guestFp, void *hostXsave, uint64_t hostCr0);
extern "C" void amdhv_svm_vmrun_guest(uint64_t guestPa, svm::GuestGprs *gprs, uint64_t hostPa,
                                      uint64_t breakpoint, uint64_t flags);
extern "C" int cpu_number(void); // com.apple.kpi.unsupported
namespace amdhv {
using namespace svm;
static bool fastRun=false;
bool fastRunEnabled() { return __atomic_load_n(&fastRun,__ATOMIC_ACQUIRE); }
void setFastRunEnabled(bool enabled) { __atomic_store_n(&fastRun,enabled,__ATOMIC_RELEASE); }
static bool fullExitTrace=false;
bool fullExitTraceEnabled() { return __atomic_load_n(&fullExitTrace,__ATOMIC_ACQUIRE); }
void setFullExitTraceEnabled(bool enabled) { __atomic_store_n(&fullExitTrace,enabled,__ATOMIC_RELEASE); }
namespace {
struct HostCaps {
    bool ready, xsave, xstateLayoutValid, hasXss, hasXfd;
    uint32_t edxA, nasid;
    uint64_t nativeCr4Features, switchMask;
    SpeculationCaps spec;
};
HostCaps gCaps = {};
uint64_t gFlushEpoch = 1;
// AMD APM vol.2 15.14: counter reloaded on VMRUN; PAUSE exits only after this
// many closely spaced hints, so a contended guest spinlock no longer exits per PAUSE.
constexpr uint16_t PauseFilterCount = 3000, PauseFilterThreshold = 128;
constexpr uint32_t VMCB_PAUSE_THRESHOLD = 0x03c, VMCB_PAUSE_COUNT = 0x03e;
// With nested paging the guest owns CR2 and CR3: reads/writes need no exit.
constexpr uint32_t NativeCrPassthrough = (1u<<2)|(1u<<3)|(1u<<18)|(1u<<19);
// The first private ASID. ASID 1 stays with the self-test and shared fallback.
constexpr uint32_t FirstPrivateAsid = 2;
}
void hostCapsInit() {
    if (gCaps.ready) return;
    HostCaps c = {};
    uint32_t r[4];
    amdhv_cpuid(0x80000008,0,r);const uint32_t ebx8=r[1];
    amdhv_cpuid(0x8000000a,0,r);c.nasid=r[1];c.edxA=r[3];
    amdhv_cpuid(0x80000000,0,r);const uint32_t maxExt=r[0];
    uint32_t eax21=0;
    if(maxExt>=0x80000021){amdhv_cpuid(0x80000021,0,r);eax21=r[0];}
    amdhv_cpuid(7,0,r);
    c.spec=speculationCaps(ebx8,c.edxA,eax21,r[3]);
    c.nativeCr4Features=supportedNativeCr4(r[1],r[2])|((r[2]&8)?NativePkeCr4:0);
    amdhv_cpuid(1,0,r);c.xsave=(r[2]&(1u<<26))!=0;
    amdhv_cpuid(0xd,0,r);c.switchMask=r[0]&UserXstateMask;
    XstateComponent parts[5]={};unsigned count=0;
    for(unsigned id=2;id<=9;++id)if((c.switchMask&(1ull<<id)) && count<5) {
        amdhv_cpuid(0xd,id,r);
        parts[count++]={id,r[0],r[1],r[2]};
    }
    c.xstateLayoutValid=validXstateLayout(c.switchMask,parts,count);
    amdhv_cpuid(0xd,1,r);c.hasXss=(r[0]&8)!=0;c.hasXfd=(r[0]&16)!=0;
    c.ready=true;
    gCaps=c;
    klog("caps: svm=%08x asids=%u switch=%llx layout=%u cr4=%llx",c.edxA,c.nasid,c.switchMask,
        c.xstateLayoutValid?1u:0u,c.nativeCr4Features);
}
void bumpFlushEpoch() { __atomic_add_fetch(&gFlushEpoch,1,__ATOMIC_SEQ_CST); }
void VcpuSvm::assignAsid(unsigned index) {
    // Valid ASIDs are 1..nasid-1. Fall back to the shared, always-flushed ASID.
    privateAsid_ = gCaps.ready && FirstPrivateAsid + index < gCaps.nasid;
    asid_ = privateAsid_ ? FirstPrivateAsid + index : 1;
    needFlush_ = true; lastCpu_ = -1;
}
void logVcpuState(const VcpuState &s) {
    klog("vcpu: exception bitmap=%llx pfmask=%llx pfmatch=%llx",s.get(0x4004),s.get(0x4006),s.get(0x4008));
    klog("vcpu: interruptibility=%llx", s.get(0x4824));
    klog("vcpu: state cr0=%llx cr3=%llx cr4=%llx efer=%llx rip=%llx rflags=%llx csar=%llx",
        s.get(0x6800), s.get(0x6802), s.get(0x6804), s.get(0x2806), s.get(0x681e), s.get(0x6820), s.get(0x4816));
    klog("vcpu: entry flags=%llx fp_invalid=%u dr7=%llx xcr0=%llx pat=%llx",
        s.lastSharedFlags, s.invalidFp ? 1u : 0u, s.get(0x681a), s.registers[50], s.get(0x2802));
    klog("vcpu: controls pin=%llx primary=%llx secondary=%llx entry=%llx exit=%llx injection=%llx",
        s.get(0x4000), s.get(0x4002), s.get(0x401e), s.get(0x4012), s.get(0x400c), s.get(0x4016));
    klog("vcpu: CR masks=%llx/%llx shadows=%llx/%llx GDTR=%llx/%llx IDTR=%llx/%llx",
        s.get(0x6000), s.get(0x6002), s.get(0x6004), s.get(0x6006), s.get(0x6816), s.get(0x4810), s.get(0x6818), s.get(0x4812));
    for (unsigned i = 0; i < 8; ++i)
        klog("vcpu: segment%u selector=%llx base=%llx limit=%llx ar=%llx", i,
            s.get(0x800+i*2), s.get(0x6806+i*2), s.get(0x4800+i*2), s.get(0x4814+i*2));
}
bool VcpuSvm::init() {
    // guest VMCB + host VMSAVE + HSAVE + IOPM(3) + MSRPM(2) + guest FP + host XSAVE
    memory_ = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous, 10 * PAGE_SIZE, 0x000ffffffffff000ull);
    if (!memory_) return false;
    if (memory_->prepare() != kIOReturnSuccess) { memory_->release(); memory_ = nullptr; return false; }
    IOByteCount size;
    physical_ = memory_->getPhysicalSegment(0, &size, kIOMemoryMapperNone);
    bytes_ = static_cast<uint8_t *>(memory_->getBytesNoCopy());
    if (!physical_ || size < 10 * PAGE_SIZE) { release(); return false; }
    bzero(bytes_, 10 * PAGE_SIZE);
    memset(bytes_ + 3 * PAGE_SIZE, 0xff, 5 * PAGE_SIZE);
    return true;
}
void VcpuSvm::release() {
    if(successfulExits_)klog("vcpu: exit trace total=%llu emitted=%llu sampled=%llu",
        successfulExits_,tracedExits_,successfulExits_-tracedExits_);
    if(runs_)klog("vcpu: asid=%u private=%u runs=%llu tlb_flushes=%llu forwarded_msrs=%llu",
        asid_,privateAsid_?1u:0u,runs_,tlbFlushes_,forwardedMsrs_);
    successfulExits_=tracedExits_=runs_=tlbFlushes_=forwardedMsrs_=0;
    if (memory_) { memory_->complete(); memory_->release(); memory_ = nullptr; }
    bytes_ = nullptr; physical_ = 0;
}
kern_return_t VcpuSvm::run(VcpuState &s, GuestMemory &memory, VcpuInterrupts &requests, unsigned cpuId) {
    const bool fast=fastRunEnabled();
    hostOnlyExit_ = false;
    uint64_t npt = memory.rootPa();
    constexpr kern_return_t Unsupported = (kern_return_t)0xfae9400f;
    constexpr kern_return_t Error = (kern_return_t)0xfae94001;
    // Cancellation is a host exit, never injection of an interrupt into the guest.
    if (requests.consume(cpuId)) {
        s.set(0x4402, 1); s.set(0x440c, 0); s.set(0x6400, 0); s.set(0x4404, 0); s.set(0x4406,0); s.set(0x440e, 0);
        s.tscSnapshot = amdhv_read_tsc();
        return 0;
    }
    // Only validated mode presets, including flat CPL3 in native fast code64.
    if (!memory_ || !npt || !s.tscSupported || s.invalidFp || !validPilotMode(s,fast)) {
        logVcpuState(s);
        return Unsupported;
    }
    // No unsupported control is silently advertised/executed.
    if (!validPilotControls(s,fast)) { logVcpuState(s); return Unsupported; }
    if(cpuId && !(s.get(0x6800)&1) && !s.loggedApEntry) {
        s.loggedApEntry=true;
        // fetchGuestCode accepts at most one x86 instruction (15 bytes).
        // Keep the final padding byte zero for the two-word diagnostic below.
        uint8_t code[16]={};const bool readable=fetchGuestCode(memory,s,code,sizeof(code)-1);
        uint64_t words[2]={};memcpy(words,code,sizeof(code));
        klog("ap-entry: id=%u readable=%u bytes=15 cs=%llx base=%llx ip=%llx code=%016llx/%016llx",
            cpuId,readable?1u:0u,s.get(0x802),s.get(0x6808),s.get(0x681e),words[0],words[1]);
        logVcpuState(s);
    }
    const bool nativeFaults=nativeFaultMode(s,fast);
    const HostCaps &caps=gCaps;
    if(!caps.ready)return Unsupported;
    const uint32_t edxA=caps.edxA;
    const bool virtualNmi=nativeFaults && (edxA&FEAT_VNMI) && (edxA&FEAT_VGIF);
    if(needsVirtualNmi(s) && !virtualNmi)return Unsupported;
    const auto specCaps=caps.spec;
    if(s.specCtrl && (!nativeFaults || !specCaps.virtualControls || (s.specCtrl&~specCaps.controls)))return Unsupported;
    uint64_t nativeCr4=nativeFaults?(caps.nativeCr4Features|(caps.xsave?NativeXsaveCr4:0)):0;
    if(s.get(0x6804)&(NativeCr4Mask|NativeXsaveCr4|NativePkeCr4)&~nativeCr4) {
        klog("vcpu: guest CR4 requests unavailable native feature");return Unsupported;
    }
    for (unsigned id = 1; id <= ManagedMsrCount; ++id)
        if (!validManagedMsr(id, s.msrValue(id))) return Unsupported;
    // Deliver the supported same-ring event in software, before TF is added.
    // Never enter a hardware interrupt gate with internal TF in the saved frame.
    if(!fast && s.get(0x4016)) {
        if(!emulateExternalEntry(memory,s)) {
            klog("vcpu: event delivery rejected info=%llx rip=%llx rsp=%llx idt=%llx",
                 s.get(0x4016),s.get(0x681e),s.get(0x681c),s.get(0x6818));
            return Unsupported;
        }
        s.set(0x4408,0);s.set(0x440a,0);
    }
    uint8_t *v = bytes_;
    bzero(v, PAGE_SIZE);
    // Fast code64 runs CR2/CR3 natively under NPT (no exit per page fault or
    // context switch). CR0/CR4/CR8 keep their mask/shadow emulation, and every
    // other mode keeps the complete CR intercept set.
    vmcb_set<uint32_t>(v, VMCB_INTERCEPT_CR, nativeFaults?~NativeCrPassthrough:0xffffffffu);
    vmcb_set<uint32_t>(v, VMCB_INTERCEPT_DR, 0xffffffffu);
    vmcb_set<uint32_t>(v, VMCB_INTERCEPT_EXC,nativeFaults?faultIntercepts(s):0xffffffffu);
    // All defined word-3 intercepts, plus VMRUN..XSETBV in word 4.
    // In particular POPF/IRET cannot disable the internal step budget.
    // Relative clock is raw host TSC (commpage tuple 0,0) plus caller offset.
    // RDTSC can execute directly in that mode; RDTSCP remains intercepted.
    // SGDT/SIDT/LGDT/LIDT operate on guest state; VMRUN/VMEXIT swaps host GDTR/IDTR.
    // Their memory operands and LTR's busy-bit store pass through NPT.
    // VMLOAD/VMSAVE inside GIF0 swaps guest/host LDTR and TR hidden caches.
    uint32_t misc1 = 0xffffffffu & ~((1u<<6) | (1u<<7) | (1u<<8) | (1u<<9) | (1u<<10) | (1u<<11) | (1u<<12) | (1u<<13));
    if(fast) misc1 &= ~((1u<<16)|(1u<<17)|(1u<<20)); // PUSHF/POPF/IRET execute without debug TF
    if(nativeFaults)misc1 &= ~(1u<<21); // native INTn checks gate DPL and guest TSS before delivery
    // INVLPG only touches this guest's ASID; with NPT it needs no emulation.
    if(nativeFaults)misc1 &= ~(1u<<25);
    if (s.relativeTsc) misc1 &= ~(1u << 14);
    vmcb_set<uint32_t>(v, VMCB_INTERCEPT_MISC1, misc1);
    vmcb_set<uint64_t>(v, VMCB_TSC_OFFSET, s.tscOffset);
    vmcb_set<uint32_t>(v, VMCB_INTERCEPT_MISC2, 0x3fffu);
    vmcb_set<uint64_t>(v, VMCB_IOPM_BASE, physical_ + 3 * PAGE_SIZE);
    vmcb_set<uint64_t>(v, VMCB_MSRPM_BASE, physical_ + 6 * PAGE_SIZE);
    vmcb_set<uint32_t>(v, VMCB_ASID, asid_);
    // Host IRQs already bound every fast quantum, so PAUSE need not exit each time.
    if(fast && (edxA&FEAT_PAUSE_FILT)) {
        vmcb_set<uint16_t>(v, VMCB_PAUSE_COUNT, PauseFilterCount);
        if(edxA&FEAT_PAUSE_THR)vmcb_set<uint16_t>(v, VMCB_PAUSE_THRESHOLD, PauseFilterThreshold);
    }
    uint64_t vintr=VINTR_MASKING | (s.registers[49]>>4);
    // vNMI is paired with vGIF; guest GIF stays open because CLGI/STGI remain
    // intercepted/unsupported. Host physical GIF still uses the assembly protocol.
    if(virtualNmi)vintr |= VirtualNmiEnable | VirtualGifEnable | VirtualGifOpen |
        ((s.get(0x4824)&8)?VirtualNmiBlocking:0);
    if(fast && (s.get(0x4002)&4))vintr |= (1ull<<8)|(1ull<<20); // VINTR intercept, ignore TPR
    vmcb_set<uint64_t>(v, VMCB_VINTR, vintr);
    if(fast)vmcb_set<uint64_t>(v,VMCB_EVENTINJ,svmEntryEvent(s));
    vmcb_set<uint32_t>(v, VMCB_INT_SHADOW, (s.get(0x4824)&3) ? 1 : 0);
    vmcb_set<uint64_t>(v, VMCB_NP_ENABLE, 1);
    vmcb_set<uint64_t>(v, VMCB_NCR3, npt);
    for (unsigned i = 0; i < 6; ++i)
        vmcb_set_seg(v, VMCB_ES + i * 16, (uint16_t)s.get(0x800 + i * 2),
            svmSegmentAttributes(s.get(0x4814 + i * 2)),
            (uint32_t)s.get(0x4800 + i * 2), s.get(0x6806 + i * 2));
    vmcb_set_seg(v, VMCB_GDTR, 0, 0, (uint32_t)s.get(0x4810), s.get(0x6816));
    vmcb_set_seg(v, VMCB_IDTR, 0, 0, (uint32_t)s.get(0x4812), s.get(0x6818));
    for(unsigned i=6;i<8;++i)
        vmcb_set_seg(v,i==6?VMCB_LDTR:VMCB_TR,(uint16_t)s.get(0x800+i*2),
            svmSegmentAttributes(s.get(0x4814+i*2)),(uint32_t)s.get(0x4800+i*2),s.get(0x6806+i*2));
    vmcb_set<uint64_t>(v, VMCB_EFER, s.get(0x2806) | EFER_SVME);
    vmcb_set<uint8_t>(v,VMCB_CPL,uint8_t(guestCpl(s)));
    if(nativeFaults && specCaps.virtualControls)vmcb_set<uint64_t>(v,VMCB_SPEC_CTRL,s.specCtrl);
    // Legacy or validated extended user-state switch runs inside GIF0. Force NE so x87
    // faults use the guest #MF path, never host IRQ13. Step remains legacy-only.
    vmcb_set<uint64_t>(v, VMCB_CR0, s.get(0x6800) | 0x30);
    vmcb_set<uint64_t>(v, VMCB_CR2, s.registers[38]);
    vmcb_set<uint64_t>(v, VMCB_CR3, s.get(0x6802));
    vmcb_set<uint64_t>(v, VMCB_CR4, s.get(0x6804) & ~0x2000ull);
    vmcb_set<uint64_t>(v, VMCB_DR7, 0x400);
    vmcb_set<uint64_t>(v, VMCB_DR6, 0xffff0ff0);
    vmcb_set<uint64_t>(v, VMCB_RFLAGS, s.get(0x6820) | (fast?2:0x102));
    vmcb_set<uint64_t>(v, VMCB_RIP, s.get(0x681e));
    vmcb_set<uint64_t>(v, VMCB_RSP, s.get(0x681c));
    vmcb_set<uint64_t>(v, VMCB_RAX, s.registers[2]);
    vmcb_set<uint64_t>(v, VMCB_G_PAT, 0x0007040600070406ull);
    for (unsigned id = 1; id <= ManagedMsrCount; ++id)
        if (managedMsrOffset(id)) vmcb_set<uint64_t>(v, managedMsrOffset(id), s.msrValue(id));
    GuestGprs g{s.registers[5],s.registers[3],s.registers[4],s.registers[6],s.registers[7],s.registers[9],
        s.registers[10],s.registers[11],s.registers[12],s.registers[13],s.registers[14],s.registers[15],s.registers[16],s.registers[17]};
    memcpy(bytes_ + 8 * PAGE_SIZE, s.fp, sizeof(s.fp));
    if(nativeFaults)memcpy(bytes_+8*PAGE_SIZE+512,s.xsaveTail,sizeof(s.xsaveTail));
    boolean_t interrupts = ml_set_interrupts_enabled(false);
    uint64_t hostCr0, hostCr4;
    asm volatile("mov %%cr0, %0" : "=r"(hostCr0));
    asm volatile("mov %%cr4, %0" : "=r"(hostCr4));
    // Legacy mode touches only legacy components; native code64 snapshots all
    // selected physical user state, including components currently disabled by host XCR0.
    if ((hostCr0 & 4) || (hostCr4 & 0x40200) != 0x40200) {
        ml_set_interrupts_enabled(interrupts); return Unsupported;
    }
    uint32_t xlo, xhi;
    asm volatile("xgetbv" : "=a"(xlo), "=d"(xhi) : "c"(0));
    if ((xlo & 3) != 3) { ml_set_interrupts_enabled(interrupts); return Unsupported; }
    uint64_t switchMask=7;
    if(nativeFaults) {
        switchMask=caps.switchMask;
        if(!caps.xstateLayoutValid || xhi ||
           !validGuestXcr0(xlo,switchMask) || !validGuestXcr0(s.registers[50],switchMask)) {
            ml_set_interrupts_enabled(interrupts);
            klog("vcpu: unsupported xstate layout/mask host=%x guest=%llx switch=%llx",xlo,s.registers[50],switchMask);return Unsupported;
        }
        if(!(switchMask&0x200))nativeCr4&=~NativePkeCr4;
        if((s.get(0x6804)&NativePkeCr4) && !(nativeCr4&NativePkeCr4)) {
            ml_set_interrupts_enabled(interrupts);return Unsupported;
        }
        // Guest XSAVES must not expose host supervisor state; a host XFD
        // restriction must not produce unexpected guest #NM. Guest MSR writes
        // remain intercepted, so neither register can be changed by this guest.
        const uint64_t hostXss=caps.hasXss?amdhv_rdmsr(0xda0):0;
        const uint64_t hostXfd=caps.hasXfd?amdhv_rdmsr(0x1c4):0;
        if(hostXss || hostXfd) {
            ml_set_interrupts_enabled(interrupts);
            klog("vcpu: extended switch requires host XSS/XFD=0, actual=%llx/%llx",hostXss,hostXfd);return Unsupported;
        }
    }
    uint64_t efer = amdhv_rdmsr(MSR_EFER), hsave = amdhv_rdmsr(MSR_VM_HSAVE_PA);
    const bool checkHostSpec=nativeFaults && specCaps.virtualControls && specCaps.controls;
    const uint64_t hostSpecCtrl=checkHostSpec?amdhv_rdmsr(0x48):0;
    if ((efer & EFER_SVME) || (amdhv_rdmsr(MSR_VM_CR) & VM_CR_SVMDIS)) {
        ml_set_interrupts_enabled(interrupts); return Unsupported;
    }
    uint64_t dr0, dr6, dr7 = amdhv_read_dr7(), cr2;
    asm volatile("mov %%dr0, %0" : "=r"(dr0));
    asm volatile("mov %%dr6, %0" : "=r"(dr6));
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    amdhv_wrmsr(MSR_EFER, (efer | EFER_SVME) & ~(1ull << 14)); // FFXSR must not skip XMM
    amdhv_wrmsr(MSR_VM_HSAVE_PA, physical_ + 2 * PAGE_SIZE);
    uint64_t exit = EXIT_INVALID,preEntryCr2=s.registers[38];
    bool canceled = false, crExit = false;
    uint64_t crQual = 0, rejectedCrValue = 0;
    unsigned rejectedCr = 0; bool crRejected = false;
    unsigned crLength = 0;
    unsigned budget = 0;
    const unsigned limit=fast?32:1024;
    // This quantum stays on one physical CPU until host state is restored.
    // The backend lease excludes NPT edits/lifetime changes, even when separate
    // vCPUs run in parallel. Decide here, with migration impossible, whether the
    // translations cached under this vCPU's ASID on this CPU can still be used.
    const int hostCpu = cpu_number();
    const uint64_t generation = memory.generation();
    const uint64_t epoch = __atomic_load_n(&gFlushEpoch,__ATOMIC_SEQ_CST);
    if (!nativeFaults || !privateAsid_ || hostCpu != lastCpu_ ||
        generation != seenGeneration_ || epoch != seenEpoch_ ||
        s.get(0x6800) != lastCr0_ || s.get(0x6802) != lastCr3_ ||
        s.get(0x6804) != lastCr4_ || s.get(0x2806) != lastEfer_) needFlush_ = true;
    lastCpu_ = hostCpu; seenGeneration_ = generation; seenEpoch_ = epoch;
    // 3 = this ASID only (FlushByAsid); 1 = everything, as before.
    const uint8_t flushControl = (privateAsid_ && (edxA&FEAT_FLUSH_ASID)) ? 3 : 1;
    ++runs_;
    // Diagnostics raised while interrupts are off are emitted after they are restored.
    enum { LogNone, LogPrivileged, LogXsetbv, LogMsrForward } deferred = LogNone;
    uint64_t logA = 0, logB = 0, logC = 0, logD = 0;
    bool specCtrlRestored = false;
    // Guest user FP state and XCR0 are loaded once for the whole quantum; the
    // lean VMRUN path below no longer switches them (nor VMSAVEs the host) per
    // entry. Only a guest change of its effective XCR0 forces a re-switch.
    uint8_t *const guestFp = bytes_ + 8 * PAGE_SIZE, *const hostFp = bytes_ + 9 * PAGE_SIZE;
    const auto effectiveXcr0 = [&]() -> uint64_t {
        return nativeFaults ? ((s.get(0x6804)&NativeXsaveCr4) ? s.registers[50] : 3) : 0;
    };
    uint64_t loadedXcr0 = effectiveXcr0();
    *reinterpret_cast<uint64_t*>(hostFp+HostXcr0Offset)=xlo;
    *reinterpret_cast<uint64_t*>(hostFp+SwitchMaskOffset)=switchMask;
    *reinterpret_cast<uint64_t*>(hostFp+GuestXcr0Offset)=loadedXcr0;
    amdhv_svm_fp_enter(guestFp, hostFp);
    bool hostSaved = false;
    // Publish the physical CPU before the first cancellation test (kick handshake).
    requests.enter(cpuId, hostCpu);
    uint64_t flagsBreak = 0, flagsNext = 0;
    for (; budget < limit; ++budget) {
        if (requests.consume(cpuId)) { canceled = true; break; }
        // No injected events in this pilot: inspect readiness at instruction
        // boundaries without arming a dummy IRQ or entering an IDT handler.
        if (!flagsBreak && !(fast && s.get(0x4016)) && (s.get(0x4002) & 4) &&
            (vmcb_get<uint64_t>(v,VMCB_RFLAGS) & 0x200) &&
            !(vmcb_get<uint32_t>(v,VMCB_INT_SHADOW) & 1)) {
            exit = 0x64; break;
        }
        vmcb_set<uint8_t>(v, VMCB_TLB_CONTROL, needFlush_ ? flushControl : 0);
        if (needFlush_) { ++tlbFlushes_; needFlush_ = false; }
        vmcb_set<uint32_t>(v, VMCB_CLEAN_BITS, 0);
        preEntryCr2=vmcb_get<uint64_t>(v,VMCB_CR2);
        if (const uint64_t wanted = effectiveXcr0(); wanted != loadedXcr0) {
            // Emulated XSETBV or CR4.OSXSAVE change: save under the full switch
            // mask, then reload with the new guest XCR0 (state itself is kept).
            amdhv_svm_fp_leave(guestFp, hostFp, hostCr0);
            *reinterpret_cast<uint64_t*>(hostFp+GuestXcr0Offset)=loadedXcr0=wanted;
            amdhv_svm_fp_enter(guestFp, hostFp);
        }
        amdhv_svm_vmrun_guest(physical_, &g, physical_ + PAGE_SIZE, flagsBreak,
                              (fast?1u:0u) | (hostSaved?0u:2u));
        hostSaved = true;
        exit = vmcb_get<uint64_t>(v, VMCB_EXITCODE);
        if(fast) {
            // EVENTINJ is one-shot even when an in-kernel exit is handled next.
            s.set(0x4016,0);vmcb_set<uint64_t>(v,VMCB_EVENTINJ,0);
            if(vmcb_get<uint64_t>(v,VMCB_EXITINTINFO)&0x80000000ull)break;
        }
        if(nativeFaults && vmcb_get<uint8_t>(v,VMCB_CPL)!=0) {
            if(exit==0x8d) { // XSETBV intercept precedes CPL #GP on SVM.
                if(faultIntercepts(s)&(1u<<13)) {
                    exit=EXIT_EXCP_BASE+13;
                    vmcb_set<uint64_t>(v,VMCB_EXITINFO1,0);break;
                }
                s.set(0x4016,faultExitInfo(13));s.set(0x4018,0);
                vmcb_set<uint64_t>(v,VMCB_EVENTINJ,svmEntryEvent(s));
                exit=EXIT_EXCP_BASE+1;continue; // retain pending entry if the quantum ends here
            }
            if(privilegedIntercept(exit)) {
                deferred=LogPrivileged;logA=exit;logB=vmcb_get<uint8_t>(v,VMCB_CPL);
                exit=0x1006;break;
            }
        }
        if (flagsBreak) {
            flagsBreak = 0;
            vmcb_set<uint32_t>(v, VMCB_INTERCEPT_MISC1, misc1);
            vmcb_set<uint64_t>(v, VMCB_DR7, 0x400);
            if (flagsStepBoundary(exit, vmcb_get<uint64_t>(v,VMCB_RIP), flagsNext,
                                  vmcb_get<uint64_t>(v,VMCB_DR6))) {
                uint64_t flags = vmcb_get<uint64_t>(v,VMCB_RFLAGS) & ~0x10000ull; // synthetic #DB RF
                if (flags & ~PilotFlags) { exit = 0x1002; break; }
                vmcb_set<uint64_t>(v,VMCB_RFLAGS,flags | 0x102);
                vmcb_set<uint64_t>(v,VMCB_DR6,0xffff0ff0);
                if (exit == EXIT_EXCP_BASE+1) continue;
            } else {
                // Faults before stack access completes retain their normal fault path.
                if (vmcb_get<uint64_t>(v,VMCB_RIP) == flagsNext) exit = 0x1003;
                break;
            }
        }
        if (exit == 0x70 || exit == 0x71) {
            flagsNext = vmcb_get<uint64_t>(v,VMCB_NRIP);
            const bool wide = (s.get(0x2806)&0x400) && (vmcb_get<uint16_t>(v,VMCB_CS+2)&0x200);
            if (!flagsStepTarget(vmcb_get<uint64_t>(v,VMCB_RIP),flagsNext,
                                 vmcb_get<uint64_t>(v,VMCB_CS+8),wide,
                                 vmcb_get<uint32_t>(v,VMCB_CS+4),flagsBreak)) break;
            vmcb_set<uint32_t>(v,VMCB_INTERCEPT_MISC1,misc1 & ~((1u<<16)|(1u<<17)));
            vmcb_set<uint64_t>(v,VMCB_DR7,0x401);
            vmcb_set<uint64_t>(v,VMCB_DR6,0xffff0ff0);
            vmcb_set<uint64_t>(v,VMCB_RFLAGS,vmcb_get<uint64_t>(v,VMCB_RFLAGS) & ~0x100ull);
            exit = EXIT_EXCP_BASE+1; // a quantum boundary may yield before the real instruction
            continue;
        }
        if (exit <= 0x1f) {
            unsigned cr = 0, reg = 0, length = 0; bool write = false;
            if (!decodeCrExit(exit, vmcb_get<uint64_t>(v, VMCB_EXITINFO1),
                vmcb_get<uint64_t>(v, VMCB_RIP), vmcb_get<uint64_t>(v, VMCB_NRIP), cr, reg, write, length)) break;
            if(cr==2)s.registers[38]=vmcb_get<uint64_t>(v,VMCB_CR2); // a native guest #PF can update it within this quantum
            uint64_t *gprs[] = {nullptr, &g.rcx, &g.rdx, &g.rbx, nullptr, &g.rbp, &g.rsi, &g.rdi,
                &g.r8, &g.r9, &g.r10, &g.r11, &g.r12, &g.r13, &g.r14, &g.r15};
            uint64_t value = reg == 0 ? vmcb_get<uint64_t>(v, VMCB_RAX) :
                reg == 4 ? vmcb_get<uint64_t>(v, VMCB_RSP) : *gprs[reg];
            if (!(s.get(0x2806) & 0x400)) value = static_cast<uint32_t>(value);
            CrAction action = emulateCr(s, cr, write, value,nativeCr4);
            if (action == CrAction::Reject) {
                crRejected = true; rejectedCr = cr; rejectedCrValue = value; break;
            }
            if (action == CrAction::Exit) {
                crExit = true; crQual = crQualification(cr, reg, write); crLength = length; break;
            }
            vmcb_set<uint32_t>(v,VMCB_INT_SHADOW,0); // emulated CR instruction retired
            if (write) {
                needFlush_ = true; // CR changes, including same-value MOV CR3, invalidate translations
                if (cr == 0) vmcb_set<uint64_t>(v, VMCB_CR0, s.get(0x6800) | 0x30);
                if (cr == 2) vmcb_set<uint64_t>(v, VMCB_CR2, s.registers[38]);
                if (cr == 3) vmcb_set<uint64_t>(v, VMCB_CR3, s.get(0x6802));
                if (cr == 4) vmcb_set<uint64_t>(v, VMCB_CR4, s.get(0x6804) & ~0x2000ull);
                if (cr == 8) vmcb_set<uint64_t>(v,VMCB_VINTR,(vmcb_get<uint64_t>(v,VMCB_VINTR)&~15ull)|(s.registers[49]>>4));
            }
            if (!write) {
                if (!(s.get(0x2806) & 0x400)) value = static_cast<uint32_t>(value);
                if (reg == 0) vmcb_set<uint64_t>(v, VMCB_RAX, value);
                else if (reg == 4) vmcb_set<uint64_t>(v, VMCB_RSP, value);
                else *gprs[reg] = value;
            }
            vmcb_set<uint64_t>(v, VMCB_RIP, vmcb_get<uint64_t>(v, VMCB_NRIP));
            exit = EXIT_EXCP_BASE + 1;
            continue;
        }
        if (exit == 0x77) { // PAUSE is a spin hint, with no architectural state changes.
            // Keep the intercept: consuming the hint counts against the same budget
            // and cannot stall the host in a guest spin loop. Do not advance on bad NRIP.
            uint64_t ignored;
            const bool wide = (s.get(0x2806)&0x400) && (vmcb_get<uint16_t>(v,VMCB_CS+2)&0x200);
            const uint64_t next = vmcb_get<uint64_t>(v,VMCB_NRIP);
            if (!flagsStepTarget(vmcb_get<uint64_t>(v,VMCB_RIP),next,
                                 vmcb_get<uint64_t>(v,VMCB_CS+8),wide,
                                 vmcb_get<uint32_t>(v,VMCB_CS+4),ignored)) break;
            vmcb_set<uint32_t>(v,VMCB_INT_SHADOW,0); // consumed PAUSE retires shadow
            vmcb_set<uint64_t>(v,VMCB_RIP,next);
            exit = EXIT_EXCP_BASE + 1;
            continue;
        }
        if(exit==0x89) { // WBINVD/WBNOINVD, never execute guest cache commands on the host.
            // Guest RAM is coherent and there is no assigned/noncoherent DMA.
            // VMEXIT/VMRUN plus a store fence suffice for this model. Keep the
            // intercept; a future passthrough/cache-type backend must revisit it.
            uint64_t ignored;
            const bool wide=(s.get(0x2806)&0x400) && (vmcb_get<uint16_t>(v,VMCB_CS+2)&0x200);
            const uint64_t next=vmcb_get<uint64_t>(v,VMCB_NRIP);
            if(!flagsStepTarget(vmcb_get<uint64_t>(v,VMCB_RIP),next,
                vmcb_get<uint64_t>(v,VMCB_CS+8),wide,vmcb_get<uint32_t>(v,VMCB_CS+4),ignored))break;
            asm volatile("mfence" ::: "memory");
            vmcb_set<uint32_t>(v,VMCB_INT_SHADOW,0);
            vmcb_set<uint64_t>(v,VMCB_RFLAGS,vmcb_get<uint64_t>(v,VMCB_RFLAGS)&~0x10000ull);
            vmcb_set<uint64_t>(v,VMCB_RIP,next);
            exit=EXIT_EXCP_BASE+1;continue;
        }
        if(exit==0x8d && nativeFaults) { // XSETBV: never execute guest input on the host.
            uint64_t value=(uint64_t(uint32_t(g.rdx))<<32)|uint32_t(vmcb_get<uint64_t>(v,VMCB_RAX));
            uint64_t rip=vmcb_get<uint64_t>(v,VMCB_RIP),next=vmcb_get<uint64_t>(v,VMCB_NRIP);
            if(!(s.get(0x6804)&NativeXsaveCr4) || uint32_t(g.rcx) || !validGuestXcr0(value,switchMask) ||
               vmcb_get<uint8_t>(v,0x4cb)!=0 || next<=rip || next-rip!=3) {
                deferred=LogXsetbv;logA=value;logB=uint32_t(g.rcx);logC=vmcb_get<uint8_t>(v,0x4cb);logD=switchMask;
                break;
            }
            s.registers[50]=value;
            vmcb_set<uint64_t>(v,VMCB_RIP,next);vmcb_set<uint32_t>(v,VMCB_INT_SHADOW,0);
            exit=EXIT_EXCP_BASE+1;continue;
        }
        if (exit == 0x79) { // INVLPG: conservative full flush, never host INVLPGA.
            const uint64_t rip = vmcb_get<uint64_t>(v, VMCB_RIP);
            const uint64_t next = vmcb_get<uint64_t>(v, VMCB_NRIP);
            if (next <= rip || next - rip > 15 ||
                !validManagedMsr(6,vmcb_get<uint64_t>(v,VMCB_EXITINFO1))) break;
            needFlush_ = true;
            vmcb_set<uint32_t>(v,VMCB_INT_SHADOW,0);
            vmcb_set<uint64_t>(v, VMCB_RIP, next);
            exit = EXIT_EXCP_BASE + 1; // consumed instruction still counts against the budget
            continue;
        }
        if (exit == 0x7c) { // MSR intercept: emulate only explicitly managed guest MSRs.
            uint32_t address = static_cast<uint32_t>(g.rcx);
            unsigned id = managedMsrId(address);
            bool write = (vmcb_get<uint64_t>(v, VMCB_EXITINFO1) & 1) != 0;
            if (id && managedMsrOffset(id)) s.setMsrValue(id, vmcb_get<uint64_t>(v, managedMsrOffset(id)));
            uint64_t value = ((g.rdx & 0xffffffffull) << 32) |
                (vmcb_get<uint64_t>(v, VMCB_RAX) & 0xffffffffull);
            if(nativeFaults) {
                const auto spec=speculationAction(specCaps,address,write,value);
                if(spec==SpeculationAction::Fault) {
                    if(faultIntercepts(s)&(1u<<13)) {
                        exit=EXIT_EXCP_BASE+13;vmcb_set<uint64_t>(v,VMCB_EXITINFO1,0);break;
                    }
                    s.set(0x4016,faultExitInfo(13));s.set(0x4018,0);
                    vmcb_set<uint64_t>(v,VMCB_EVENTINJ,svmEntryEvent(s));
                    exit=EXIT_EXCP_BASE+1;continue;
                }
                if(spec!=SpeculationAction::Forward) {
                    const uint64_t rip=vmcb_get<uint64_t>(v,VMCB_RIP),next=vmcb_get<uint64_t>(v,VMCB_NRIP);
                    if(next<=rip || next-rip>15){exit=0x1008;break;}
                    if(spec==SpeculationAction::Command) {
                        // A real, capability-checked barrier, never an ignored request.
                        if(value)amdhv_wrmsr(address,value);
                    } else if(spec==SpeculationAction::WriteControl) {
                        vmcb_set<uint64_t>(v,VMCB_SPEC_CTRL,value);
                    } else {
                        value=vmcb_get<uint64_t>(v,VMCB_SPEC_CTRL);
                        vmcb_set<uint64_t>(v,VMCB_RAX,uint32_t(value));g.rdx=value>>32;
                    }
                    vmcb_set<uint32_t>(v,VMCB_INT_SHADOW,0);
                    vmcb_set<uint64_t>(v,VMCB_RIP,next);
                    exit=EXIT_EXCP_BASE+1;continue;
                }
            }
            MsrAction action = emulateManagedMsr(s, address, write, value, amdhv_read_tsc());
            if (action == MsrAction::Exit) {
                if(nativeFaults){deferred=LogMsrForward;logA=write?1:0;logB=address;logC=value;}
                break;
            }
            if (action == MsrAction::Reject) { exit = 0x1000; break; }
            if (write) {
                if (id == TscMsrId) vmcb_set<uint64_t>(v, VMCB_TSC_OFFSET, s.tscOffset);
                else vmcb_set<uint64_t>(v, managedMsrOffset(id), value);
            } else {
                vmcb_set<uint64_t>(v, VMCB_RAX, static_cast<uint32_t>(value));
                g.rdx = value >> 32;
            }
            vmcb_set<uint32_t>(v,VMCB_INT_SHADOW,0);
            vmcb_set<uint64_t>(v, VMCB_RIP, vmcb_get<uint64_t>(v, VMCB_RIP) + 2);
            exit = EXIT_EXCP_BASE + 1; // budget exhaustion must not report a consumed MSR exit
            continue; // counts against the same execution budget
        }
        if (fast || exit != EXIT_EXCP_BASE + 1) break;
        // Internal #DB step. MOV SS can suppress one trap: budget counts entries, not instructions.
    }
    requests.leave(cpuId); // still interrupt-disabled: no kick can target this CPU for us now
    amdhv_svm_fp_leave(guestFp, hostFp, hostCr0); // guest FP -> buffer, host FP/XCR0/CR0 restored
    uint64_t restoredDr0;
    asm volatile("mov %%dr0, %0" : "=r"(restoredDr0));
    if (restoredDr0 != dr0) { // restore defensively before re-enabling host debug registers
        asm volatile("mov %0, %%dr0" :: "r"(dr0));
        exit = 0x1004; canceled = false; crExit = false;
    }
    asm volatile("mov %0, %%cr2" :: "r"(cr2) : "memory");
    asm volatile("mov %0, %%dr6" :: "r"(dr6));
    amdhv_write_dr7(dr7);
    amdhv_wrmsr(MSR_VM_HSAVE_PA, hsave);
    amdhv_wrmsr(MSR_EFER, efer);
    if(checkHostSpec && amdhv_rdmsr(0x48)!=hostSpecCtrl) {
        amdhv_wrmsr(0x48,hostSpecCtrl);exit=0x1007;canceled=false;crExit=false;
        specCtrlRestored=true;
    }
    ml_set_interrupts_enabled(interrupts);
    // Logging takes a mutex and may block: only with host interrupts restored.
    if(specCtrlRestored)klog("vcpu: hardware guest SPEC_CTRL did not preserve host value");
    if(deferred==LogPrivileged)klog("vcpu: refused privileged intercept=%llx at CPL%u",logA,unsigned(logB));
    else if(deferred==LogXsetbv)klog("vcpu: rejected XSETBV value=%llx index=%x cpl=%u supported=%llx",logA,uint32_t(logB),unsigned(logC),logD);
    else if(deferred==LogMsrForward) {
        // Forwarded MSRs (APIC base, x2APIC, timers) are routine and frequent:
        // keep the first few per vCPU as boot evidence, then only under full trace.
        if(++forwardedMsrs_<=64 || fullExitTraceEnabled())
            klog("vcpu: forwarding %s MSR=%x value=%llx",logA?"WRMSR":"RDMSR",uint32_t(logB),logC);
    }
    memcpy(s.fp, bytes_ + 8 * PAGE_SIZE, sizeof(s.fp));
    if(nativeFaults)memcpy(s.xsaveTail,bytes_+8*PAGE_SIZE+512,sizeof(s.xsaveTail));
    else s.legacyFpChanged();
    if(nativeFaults && specCaps.virtualControls)s.specCtrl=vmcb_get<uint64_t>(v,VMCB_SPEC_CTRL);
    s.tscSnapshot = amdhv_read_tsc();
    for (unsigned id = 1; id <= ManagedMsrCount; ++id)
        if (managedMsrOffset(id)) s.setMsrValue(id, vmcb_get<uint64_t>(v, managedMsrOffset(id)));
    s.set(0x4810, vmcb_get<uint32_t>(v, VMCB_GDTR + 4));
    s.set(0x6816, vmcb_get<uint64_t>(v, VMCB_GDTR + 8));
    s.set(0x4812, vmcb_get<uint32_t>(v, VMCB_IDTR + 4));
    s.set(0x6818, vmcb_get<uint64_t>(v, VMCB_IDTR + 8));
    // Preserve segment-cache changes across exits, even if the next entry rejects them.
    for (unsigned i = 0; i < 8; ++i) {
        unsigned off = i<6?VMCB_ES+i*16:i==6?VMCB_LDTR:VMCB_TR;
        s.set(0x800 + i * 2, vmcb_get<uint16_t>(v, off));
        const uint16_t attr=vmcb_get<uint16_t>(v,off+2);
        s.set(0x4814 + i * 2,vmxSavedCplAttributes(i,attr,vmcb_get<uint32_t>(v,off+4),vmcb_get<uint8_t>(v,VMCB_CPL)));
        s.set(0x4800 + i * 2, vmcb_get<uint32_t>(v, off + 4));
        s.set(0x6806 + i * 2, vmcb_get<uint64_t>(v, off + 8));
    }
    s.set(0x681e, vmcb_get<uint64_t>(v, VMCB_RIP));
    // Natively executed MOV CR3 updates only the VMCB. Publish it before any
    // guest page walk below, then remember the paging registers this ASID's
    // translations were built under; a later outside change forces a flush.
    if(nativeFaults)s.set(0x6802, vmcb_get<uint64_t>(v, VMCB_CR3));
    lastCr0_=s.get(0x6800);lastCr3_=s.get(0x6802);lastCr4_=s.get(0x6804);lastEfer_=s.get(0x2806);
    const bool faultExit=nativeFaults && exit>=EXIT_EXCP_BASE && exit<EXIT_EXCP_BASE+32 &&
        nativeFaultVector(unsigned(exit-EXIT_EXCP_BASE));
    // Intel direct #PF VM-exit leaves CR2 unchanged; SVM may update its copy.
    s.registers[38] = faultExit&&exit==EXIT_EXCP_BASE+14 ? preEntryCr2 : vmcb_get<uint64_t>(v,VMCB_CR2);
    s.set(0x681c, vmcb_get<uint64_t>(v, VMCB_RSP));
    s.set(0x6820, vmcb_get<uint64_t>(v, VMCB_RFLAGS) & ~0x100ull);
    // Never publish both VMX shadow bits: Apple rejects that combination.
    s.set(0x4824, (vmcb_get<uint32_t>(v,VMCB_INT_SHADOW)&1) ?
        ((s.get(0x6820)&0x200) ? 1 : 2) : 0);
    if(virtualNmi && (vmcb_get<uint64_t>(v,VMCB_VINTR)&VirtualNmiBlocking))s.set(0x4824,s.get(0x4824)|8);
    s.registers[2] = vmcb_get<uint64_t>(v, VMCB_RAX);
    s.registers[5]=g.rbx; s.registers[3]=g.rcx; s.registers[4]=g.rdx; s.registers[6]=g.rsi;
    s.registers[7]=g.rdi; s.registers[9]=g.rbp;
    s.registers[10]=g.r8; s.registers[11]=g.r9; s.registers[12]=g.r10; s.registers[13]=g.r11;
    s.registers[14]=g.r12; s.registers[15]=g.r13; s.registers[16]=g.r14; s.registers[17]=g.r15;
    s.set(0x440c, 0); s.set(0x6400, 0); s.set(0x4404, 0); s.set(0x4406,0); s.set(0x440e, 0);
    s.set(0x4408,fast?(vmcb_get<uint64_t>(v,VMCB_EXITINTINFO)&0xffffffffull):0);
    s.set(0x440a,fast?(vmcb_get<uint64_t>(v,VMCB_EXITINTINFO)>>32):0);
    if (canceled || (fast && (exit==EXIT_INTR || exit==EXIT_NMI)) || (budget == limit && exit == EXIT_EXCP_BASE + 1)) {
        s.set(0x4402, 1);
        // IDT-vectoring info (an event the IRQ interrupted mid-delivery) must
        // reach the VMM for reinjection, so that case always returns to it.
        hostOnlyExit_ = fast && !canceled && !(vmcb_get<uint64_t>(v,VMCB_EXITINTINFO)&0x80000000ull);
    }
    else if(faultExit) {
        const unsigned vector=unsigned(exit-EXIT_EXCP_BASE);
        s.set(0x4402,0);s.set(0x4404,faultExitInfo(vector));
        s.set(0x4406,faultHasError(vector)?uint32_t(vmcb_get<uint64_t>(v,VMCB_EXITINFO1)):0);
        if(vector==14)s.set(0x6400,vmcb_get<uint64_t>(v,VMCB_EXITINFO2));
    }
    else if(nativeFaults&&exit==EXIT_SHUTDOWN){s.set(0x4402,2);}
    else if (exit == 0x64) { s.set(0x4402, 7); }
    else if (crExit) { s.set(0x4402, 28); s.set(0x6400, crQual); s.set(0x440c, crLength); }
    else if (exit>=0x20 && exit<=0x3f) {
        // Read guest instruction bytes only after restoring host IRQs; memory
        // descriptors may block. Return after one emulated MOV DR, never recurse.
        const uint64_t rip=s.get(0x681e),next=vmcb_get<uint64_t>(v,VMCB_NRIP);
        const bool wide=(s.get(0x2806)&0x400) && (s.get(0x4816)&0x2000);
        uint8_t code[15]={}; DebugMove move;
        if(next<=rip || next-rip>15 || !fetchGuestCode(memory,s,code,unsigned(next-rip)) ||
           !decodeDebugMove(code,unsigned(next-rip),wide,exit,move)) return Unsupported;
        if(s.get(0x4002)&0x800000) {
            s.set(0x4402,29); s.set(0x440c,next-rip);
            s.set(0x6400,move.dr | (move.write?0ull:16ull) | (uint64_t(move.gpr)<<8));
        } else {
            if(!emulateDebugMove(s,move,wide)) return Unsupported;
            s.set(0x681e,next); s.set(0x4824,0); s.set(0x4402,1);
        }
    }
    else if (exit==0x74) {
        // Fast IRET executes in hardware (intercept cleared above). Never use
        // the step runner's software PTE/frame transaction under a shared lease.
        if(fast)return Unsupported;
        uint8_t code[2]={};
        if(!fetchGuestCode(memory,s,code,2) || code[0]!=0x48 || code[1]!=0xcf || !emulateIret64(memory,s)) {
            klog("vcpu: IRETQ rejected rip=%llx rsp=%llx",s.get(0x681e),s.get(0x681c));return Unsupported;
        }
        s.set(0x4402,1);
    }
    else if(exit==0x76) { // INVD always exits in VMX; leave emulation to the VMM.
        uint64_t ignored;const bool wide=(s.get(0x2806)&0x400) && (s.get(0x4816)&0x2000);
        const uint64_t rip=s.get(0x681e),next=vmcb_get<uint64_t>(v,VMCB_NRIP);
        if(!flagsStepTarget(rip,next,s.get(0x6808),wide,s.get(0x4802),ignored))return Unsupported;
        s.set(0x4402,13);s.set(0x440c,next-rip);
    }
    else if (exit == EXIT_CPUID) { s.set(0x4402, 10); s.set(0x440c, 2); }
    else if (exit == EXIT_HLT) { s.set(0x4402, 12); s.set(0x440c, 1); }
    else if (exit == EXIT_NPF) {
        uint64_t info = vmcb_get<uint64_t>(v, VMCB_EXITINFO1);
        s.set(0x4402, 48); s.set(0x2400, vmcb_get<uint64_t>(v, VMCB_EXITINFO2));
        s.set(0x6400, (info & 16) ? 4 : (info & 2) ? 2 : 1);
    } else if (exit == 0x7b) {
        uint64_t rip=s.get(0x681e), next=vmcb_get<uint64_t>(v,VMCB_EXITINFO2);
        uint8_t code[15] = {}; IoExit io;
        unsigned bits=(s.get(0x2806)&0x400) && (s.get(0x4816)&0x2000) ? 64 : (s.get(0x4816)&0x4000)?32:16;
        if (next<=rip || next-rip>15 || !fetchGuestCode(memory,s,code,(unsigned)(next-rip)) ||
            !translateIo(vmcb_get<uint64_t>(v,VMCB_EXITINFO1),code,(unsigned)(next-rip),bits,(uint16_t)s.registers[4],io)) {
            klog("vcpu: IO decode rejected rip=%llx info=%llx next=%llx",rip,vmcb_get<uint64_t>(v,VMCB_EXITINFO1),next);
            return Unsupported;
        }
        s.set(0x4402,30); s.set(0x440c,next-rip); s.set(0x6400,io.qualification); s.set(0x440e,io.instructionInfo);
    } else if (exit == 0x7c) {
        s.set(0x4402, (vmcb_get<uint64_t>(v, VMCB_EXITINFO1) & 1) ? 32 : 31);
        s.set(0x440c, 2);
    } else {
        if (exit == 0x1002 || exit == 0x1003)
            klog("vcpu: flags-step next=%llx dr6=%llx flags=%llx",flagsNext,
                vmcb_get<uint64_t>(v,VMCB_DR6),vmcb_get<uint64_t>(v,VMCB_RFLAGS));
        if (crRejected) klog("vcpu: rejected CR%u operand=%llx", rejectedCr, rejectedCrValue);
        klog("vcpu: bounded run stopped id=%u exit=0x%llx rip=0x%llx", cpuId, exit, s.get(0x681e));
        klog("vcpu: exit info1=%llx info2=%llx nrip=%llx csbase=%llx cr0=%llx cr4=%llx",
            vmcb_get<uint64_t>(v, VMCB_EXITINFO1), vmcb_get<uint64_t>(v, VMCB_EXITINFO2),
            vmcb_get<uint64_t>(v, VMCB_NRIP), s.get(0x6808),
            vmcb_get<uint64_t>(v, VMCB_CR0), vmcb_get<uint64_t>(v, VMCB_CR4));
        klog("vcpu: exit exception bitmap=%llx pfmask=%llx pfmatch=%llx entry=%llx flags=%llx",
            s.get(0x4004),s.get(0x4006),s.get(0x4008),s.get(0x4016),s.get(0x6820));
        logVcpuState(s);
        uint8_t code[16]={};const bool readable=fetchGuestCode(memory,s,code,15);
        uint64_t words[2]={};memcpy(words,code,sizeof(code));
        klog("vcpu: stopped code15 readable=%u bytes=%016llx/%016llx",readable?1u:0u,words[0],words[1]);
        klog("vcpu: stopped ax=%llx bx=%llx cx=%llx dx=%llx si=%llx di=%llx bp=%llx",
            s.registers[2],s.registers[5],s.registers[3],s.registers[4],s.registers[6],s.registers[7],s.registers[9]);
        // Read-only evidence for the observed compatibility-mode far-jump #GP.
        // Walk data permissions, not execute permissions; never commit A/D updates.
        if(readable && code[0]==0xff && code[1]==0x2f && (s.get(0x6800)&0x80000000) &&
           pagingEfer(s.get(0x2806))==0x500 && !(s.get(0x4816)&0x2000)) {
            const uint64_t address=s.get(0x680c)+uint32_t(s.registers[7]);
            EventMemoryPlan plan;uint8_t ptr[6]={};bool pointerRead=true;
            for(unsigned i=0;i<sizeof(ptr);++i){uint64_t pa=0;
                if(!plan.translate(memory,s,address+i,false,pa)||!memory.readPhysical(pa,ptr+i,1)){pointerRead=false;break;}}
            uint32_t target=0;uint16_t selector=0;memcpy(&target,ptr,4);memcpy(&selector,ptr+4,2);
            uint64_t descriptor=0;bool descriptorRead=pointerRead && !(selector&4) &&
                uint64_t(selector&~7u)+7<=s.get(0x4810);
            if(descriptorRead){uint8_t *out=reinterpret_cast<uint8_t*>(&descriptor);
                for(unsigned i=0;i<8;++i){uint64_t pa=0;
                    if(!plan.translate(memory,s,s.get(0x6816)+(selector&~7u)+i,false,pa)||!memory.readPhysical(pa,out+i,1)){descriptorRead=false;break;}}}
            klog("vcpu: far-jump ptr=%llx readable=%u target=%x selector=%x descriptor_readable=%u descriptor=%llx",
                address,pointerRead?1u:0u,target,unsigned(selector),descriptorRead?1u:0u,descriptor);
        }
        return exit == EXIT_INVALID ? Error : Unsupported;
    }
    // Routine exits can dominate SMP spin loops. Avoid IOLog and log-buffer
    // locking on every successful fast quantum. Always retain exception and
    // shutdown exits, the first 32, and logarithmic progress per vCPU. Rejected
    // states/unsupported exits above are unconditional, including code/GPR dumps.
    // Full tracing can be enabled live without changing guest execution policy.
    ++successfulExits_;
    if(!fast || fullExitTraceEnabled() || s.get(0x4402)==0 || s.get(0x4402)==2 ||
       successfulExits_<=32 || !(successfulExits_&(successfulExits_-1))) {
        ++tracedExits_;
        klog("vcpu: id=%u exit#=%llu SVM=0x%llx -> VMX=%llu rip=0x%llx",
            cpuId,successfulExits_,exit,s.get(0x4402),s.get(0x681e));
    }
    return 0;
}
}
