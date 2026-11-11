#pragma once
#include "vcpu_state.hpp"
#include "cr_policy.hpp"
namespace amdhv {
// Synchronous architectural faults handled by the guest IDT in fast code64.
// Debug, NMI, machine-check, reserved and vendor-specific vectors stay trapped.
constexpr uint32_t NativeFaultMask=(1u<<0)|(1u<<6)|(1u<<7)|(1u<<8)|(1u<<10)|
    (1u<<11)|(1u<<12)|(1u<<13)|(1u<<14)|(1u<<16)|(1u<<17)|(1u<<19);
// Software traps execute natively with their architectural post-instruction RIP.
// Their intercepted VMX exit/type6 reinjection ABI remains unsupported.
constexpr uint32_t NativeTrapMask=1u<<3;
constexpr bool nativeFaultVector(unsigned v){return v<32 && (NativeFaultMask&(1u<<v));}
constexpr bool faultHasError(unsigned v){return v==8 || (v>=10&&v<=14) || v==17;}
inline bool nativeFaultMode(const VcpuState &s,bool fast){
    return fast && pagingEfer(s.get(0x2806))==0x500 && (s.get(0x4816)&0x2000);
}
inline bool validFaultControls(const VcpuState &s){
    // Nonconstant PF filters need dynamic interception/reflection, not implemented.
    return s.get(0x4004)<=0xffffffffull && !s.get(0x4006) && s.get(0x4008)<=0xffffffffull;
}
inline uint32_t faultIntercepts(const VcpuState &s){
    uint32_t bits=~(NativeFaultMask|NativeTrapMask) | uint32_t(s.get(0x4004));
    // With mask0, equality is constant: match0 follows the bitmap; nonzero
    // match reverses it, as required by VMX's PF error mask/match semantics.
    if(s.get(0x4008))bits^=1u<<14;
    return bits;
}
inline bool validFaultEntry(const VcpuState &s){
    uint64_t info=s.get(0x4016);unsigned v=unsigned(info&255);
    if(!nativeFaultVector(v))return false;
    return info==(0x80000300ull|v|(faultHasError(v)?0x800ull:0)) &&
        (!faultHasError(v) || s.get(0x4018)<=0xffffffffull) &&
        ((v!=8&&v!=17) || !s.get(0x4018));
}
inline uint64_t svmEntryEvent(const VcpuState &s){
    return s.get(0x4016)|((s.get(0x4016)&0x800)?s.get(0x4018)<<32:0);
}
constexpr uint32_t faultExitInfo(unsigned v){return 0x80000300u|v|(faultHasError(v)?0x800u:0);}
}
