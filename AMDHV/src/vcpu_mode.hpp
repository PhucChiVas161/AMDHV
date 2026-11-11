#pragma once
#include "vcpu_state.hpp"
#include "cr_policy.hpp"
#include "flags_step.hpp"
#include "debug_emulation.hpp"
#include "event_emulation.hpp"
#include "system_segments.hpp"
#include "segment_attributes.hpp"
#include "exception_policy.hpp"
#include "cpl_policy.hpp"
#include "nmi_policy.hpp"
namespace amdhv {
inline bool validPilotControls(const VcpuState &s,bool fast=false) {
    const unsigned fixedControls[] = {0x4000, 0x400c, 0x401e};
    for (unsigned enc : fixedControls)
        if (s.get(enc)) return false;
    if(!validFaultControls(s))return false;
    if(!validExternalEntry(s) && !(nativeFaultMode(s,fast)&&validFaultEntry(s)) &&
       !validNativeNmiEntry(s,fast))return false;
    uint64_t primary = s.get(0x4002);
    if (primary & ~0xb80084ull) return false;
    // IA-32e guest entry is meaningful only with active long mode. Existing
    // direct-SVM pilot callers may leave this VMX-only control zero.
    uint64_t entry = s.get(0x4012);
    if ((entry & ~0x200ull) || ((entry & 0x200) && pagingEfer(s.get(0x2806)) != 0x500)) return false;
    // CR8 is emulated against the framework TPR register; virtual APIC pages
    // remain unsupported. Only explicit external IRQ entry is supported.
    // Any nonzero threshold still fails closed.
    if ((primary & 0x200000) && s.get(0x401c)) return false;
    return true;
}
inline bool validPilotMode(const VcpuState &s,bool fast=false) {
    const uint64_t cr0 = s.get(0x6800), cr4 = s.get(0x6804), efer = pagingEfer(s.get(0x2806));
    // Apple's VMX reset state retains NE and the host-owned VMXE bit. VMXE is
    // never loaded into AMD CR4, and MOV CR reads use mask/shadow emulation.
    const bool native64=fast && efer==0x500 && (s.get(0x4816)&0x2000);
    const bool cr4OK = validLegacyCr4(cr4, s.get(0x6002), s.get(0x6006),native64?(NativeCr4Mask|NativeXsaveCr4|NativePkeCr4):0);
    const bool armed = efer == 0x100 && (cr0 & 1) && (cr4 & 0x20);
    const bool legacy = validLegacyCr0(cr0) && cr4OK && (efer == 0 || armed);
    const bool real = legacy && !(cr0 & 1);
    const bool flat32 = legacy && (cr0 & 1);
    const bool long64 = (cr0 & 0x80000001ull) == 0x80000001ull &&
        validLegacyCr0(cr0 & ~0x80000000ull) && cr4OK && (cr4 & 0x20) && efer == 0x500;
    const bool code64 = long64 && (s.get(0x4816) & 0x2000);
    // Linux AP startup copies SCE with LME/NXE before setting CR0.PG.
    // SCE does not imply LMA. Keep it out of the TF runner, but permit the
    // validated ring-0 LME preparation state as well as active IA-32e in fast mode.
    if((s.get(0x2806)&1) && (!fast || (!armed && !long64)))return false;
    if (!real && !flat32 && !long64) return false;
    // TF is internal. IOPL/VM and arbitrary debug/FPU state are not supported yet.
    if ((s.get(0x6820) & ~(PilotFlags|(native64?0x50000ull:0ull))) || !validDisabledDr7(s.get(0x681a)) ||
        (native64?!validGuestXcr0(s.registers[50]):s.registers[50]!=1))
        return false;
    const uint64_t maxAddress = real ? 0xffff : 0xffffffffull;
    if(code64) {
        if(!canonical48(s.get(0x681e)) || !canonical48(s.get(0x681c)))return false;
    } else if(s.get(0x681e)>maxAddress || s.get(0x681c)>maxAddress)return false;
    if (!validPilotCr3(s.get(0x6802))) return false;
    if (s.get(0x2802) != 0x0007040600070406ull) return false;
    const unsigned tables[] = {0x6816, 0x6818};
    for (unsigned enc : tables) {
        uint64_t base = s.get(enc);
        if (long64 ? !validManagedMsr(6, base) : base > 0xffffffffull) return false;
    }
    if (s.get(0x4810) > 0xffff || s.get(0x4812) > 0xffff) return false;
    for (unsigned i = 6; i < 8; ++i) {
        if(!validSystemSegment(s,i,long64))return false;
    }
    // These guest states are fixed in this pilot and must not be silently ignored.
    // SVM exposes one interrupt shadow bit; vNMI separately tracks NMI blocking.
    // Only native code64 may use it, with a physical-capability check in run().
    const uint64_t shadow = s.get(0x4824);
    if ((shadow & ~(native64?0xbull:3ull)) || (shadow&3)==3 ||
        ((shadow&1) && !(s.get(0x6820)&0x200)))return false;
    const unsigned fixed[] = {0x4826, 0x6822};
    for (unsigned enc : fixed)
        if (s.get(enc)) return false;
    for (unsigned i=41;i<=44;++i) if(!validManagedMsr(6,s.registers[i])) return false;
    if(s.registers[45] || s.registers[46] || !validGuestDr6(s.registers[47])) return false;
    if (s.registers[49]>255) return false;
    if(code64&&fast)return validFastCode64Segments(s);
    for (unsigned i = 0; i < 6; ++i) {
        // Code64 permits null data caches and ignores data limits/DB (FS/GS
        // base still applies). SYSCALL can also synthesize a flat data cache.
        // Keep the ring0/type/base restrictions while preserving native caches.
        if(code64 && i!=1) {
            const uint64_t ar=s.get(0x4814+i*2),base=s.get(0x6806+i*2),sel=s.get(0x800+i*2);
            if(s.get(0x4800+i*2)>0xffffffffull ||
               sel>0xffff || (sel&7) || (ar==0x10000?sel!=0:(ar&~0xc000ull)!=0x93) ||
               (i>=4?!validManagedMsr(i+2,base):base!=0))return false;
            continue;
        }
        if (flat32 || (long64 && !code64)) {
            // Includes the cached 16-bit segments immediately after setting PE,
            // and ring-0 GDT segments loaded by a subsequent far jump/MOV Sreg.
            uint64_t ar = s.get(0x4814 + i*2), limit = s.get(0x4800 + i*2);
            uint64_t access = i == 1 ? 0x9b : 0x93;
            if ((ar != access && ar != (access | 0x4000) && ar != (access | 0xc000)) ||
                s.get(0x6806 + i*2) > 0xffffffffull || (s.get(0x800 + i*2) & 7)) return false;
            if ((ar & 0x8000) ? (limit != 0xffffffffull) : (limit > 0xfffff)) return false;
            continue;
        }
        uint64_t ar = i == 1 ? (real ? 0x9b : long64 ? 0xa09b : 0xc09b) : (real ? 0x93 : 0xc093);
        if (s.get(0x4800 + i * 2) != (real ? 0xffffull : 0xffffffffull) ||
            s.get(0x4814 + i * 2) != ar) return false;
        uint64_t base = s.get(0x6806 + i * 2);
        if (long64 && i >= 4) { if (!validManagedMsr(i + 2, base)) return false; }
        else if (real ? base > 0xffffffffull : base != 0) return false;
        if (!real && (s.get(0x800 + i * 2) & 7)) return false;
    }
    return true;
}
}
