#pragma once
#include "vcpu_state.hpp"
namespace amdhv {
// Only LDT and busy 32/64-bit TSS caches. Hardware LLDT/LTR performs guest GDT
// access/type/presence checks and the TSS busy-bit write through NPT. No host
// descriptor table or selector is ever used for a guest instruction.
inline bool validSystemSegment(const VcpuState &s,unsigned i,bool longMode) {
    const uint64_t selector=s.get(0x800+i*2),base=s.get(0x6806+i*2);
    const uint64_t limit=s.get(0x4800+i*2),ar=s.get(0x4814+i*2);
    if(selector>0xffff || (selector&4) || limit>0xffffffffull ||
       (longMode?!validManagedMsr(6,base):base>0xffffffffull))return false;
    if(!(selector&0xfff8)) {
        if(i==6 && ar==0x10000)return true; // LLDT null: hidden base/limit ignored
        return !base && limit==0xffff && ar==(i==6?0x82ull:0x8bull);
    }
    const uint64_t type=i==6?0x82ull:0x8bull;
    if((ar&~0x9000ull)!=type || ((ar&0x8000)?(limit&0xfff)!=0xfff:limit>0xfffff))return false;
    return i==6 || limit>=0x67;
}
}
