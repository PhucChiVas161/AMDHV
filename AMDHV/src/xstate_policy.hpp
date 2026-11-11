#pragma once
#include <stdint.h>
namespace amdhv {
constexpr uint64_t UserXstateMask=0x2e7; // x87/SSE/AVX, AVX-512 group, PKRU
constexpr bool validGuestXcr0(uint64_t value,uint64_t supported=UserXstateMask) {
    return (value&1) && !(value&~(supported&UserXstateMask)) && (!(value&4)||(value&2)) &&
        (!(value&0xe0) || ((value&0xe4)==0xe4));
}
constexpr uint64_t NativeXsaveCr4=0x40000;
constexpr uint64_t NativePkeCr4=0x400000;
constexpr unsigned XstateLimit=3968,HostXcr0Offset=4064,SwitchMaskOffset=4072,GuestXcr0Offset=4088;
struct XstateComponent {unsigned id,size,offset,flags;};
inline bool validXstateLayout(uint64_t mask,const XstateComponent *parts,unsigned count) {
    if(!validGuestXcr0(mask) || (mask&7)!=7 || count>5)return false;
    uint64_t found=3;
    for(unsigned i=0;i<count;++i) {
        auto p=parts[i];
        if(p.id<2 || p.id>9 || !(mask&(1ull<<p.id)) || (found&(1ull<<p.id)) ||
           !p.size || p.offset<576 || p.offset>XstateLimit || p.size>XstateLimit-p.offset || (p.flags&1))return false;
        if(p.id==2 && (p.size!=256 || p.offset!=576))return false;
        for(unsigned j=0;j<i;++j)if(p.offset<parts[j].offset+parts[j].size && parts[j].offset<p.offset+p.size)return false;
        found|=1ull<<p.id;
    }
    return found==mask;
}
}
