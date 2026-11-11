#pragma once
#include "vcpu_state.hpp"
namespace amdhv {
// VMX input checks below make SS.DPL agree with CS.RPL/DPL. SVM has a separate
// CPL byte; it must never be left at zero when restoring a user context.
inline unsigned guestCpl(const VcpuState &s){return unsigned((s.get(0x4818)>>5)&3);}
inline bool validFastCode64Segments(const VcpuState &s){
    const uint64_t cs=s.get(0x802),csar=s.get(0x4816);
    const unsigned cpl=guestCpl(s);
    if((cpl!=0&&cpl!=3) || cs>0xffff || !(cs&0xfff8) || (cs&7)!=cpl ||
       csar!=(0xa09bull|(uint64_t(cpl)<<5)) || s.get(0x6808) || s.get(0x4802)!=0xffffffffull)return false;
    if(cpl==3 && !(s.get(0x80e)&0xfff8))return false; // validated long-mode TSS required
    const unsigned dataSegments[]={0,2,3,4,5};
    for(unsigned i : dataSegments) {
        const uint64_t sel=s.get(0x800+i*2),ar=s.get(0x4814+i*2),base=s.get(0x6806+i*2);
        const unsigned rpl=unsigned(sel&3),dpl=unsigned((ar>>5)&3);
        if(sel>0xffff || (sel&4) || (rpl!=0&&rpl!=3) || s.get(0x4800+i*2)>0xffffffffull ||
           (i>=4?!canonical48(base):base!=0))return false;
        if(ar&0x10000) {
            if(ar!=0x10000 || (sel&0xfffc) || (i==2&&(cpl!=0||sel)))return false;
            continue;
        }
        if(!(sel&0xfff8) || (ar&~0xc060ull)!=0x93 || (dpl!=0&&dpl!=3))return false;
        if(i==2 ? (rpl!=cpl||dpl!=cpl) : (rpl>dpl||cpl>dpl))return false;
    }
    return true;
}
// Simple CPL exceptions precede these SVM instruction intercepts. If one
// nevertheless arrives from user mode, stop before emulation or VMX forwarding.
constexpr bool privilegedIntercept(uint64_t exit){
    return exit<=0x3f || exit==0x6f || exit==0x76 || exit==0x78 || exit==0x79 ||
        exit==0x7a || exit==0x7c || exit==0x89 || exit==0x8a || exit==0x8b || exit==0x8c;
}
}
