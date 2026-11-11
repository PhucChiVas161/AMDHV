#pragma once
#include "vcpu_state.hpp"
#include "cr_policy.hpp"
#include "cpl_policy.hpp"
namespace amdhv {
// Read-only walker for the pilot's unpaged or four-level IA-32e guest. Not an
// execution emulator: only reads the exact bytes of an already intercepted instruction.
template<class Memory> bool guestCodeByte(Memory &m, const VcpuState &s, uint64_t linear, uint8_t &byte) {
    uint64_t pa = linear;
    if (s.get(0x6800) & 0x80000000) {
        if (pagingEfer(s.get(0x2806)) != 0x500 || !(s.get(0x6804) & 0x20) || !validManagedMsr(6,linear)) return false;
        uint64_t table = s.get(0x6802);
        bool user=true;
        for (int level = 3; level >= 0; --level) {
            uint64_t e=0;
            unsigned shift = 12u + 9u*static_cast<unsigned>(level);
            if (!m.readPhysical(table + ((linear>>shift)&511)*8, &e, 8) || !(e&1) ||
                (e & 0x800f000000000000ull)) return false; // NX blocks fetch when enabled; is reserved when disabled. Also reject >48-bit GPA.
            user = user && (e&4);
            if ((e&128) && level) {
                if (level==3 || (e & (((1ull<<shift)-1) & ~0x1fffull))) return false;
                pa = (e & 0x0000fffffffff000ull & ~((1ull<<shift)-1)) | (linear & ((1ull<<shift)-1));
                break;
            }
            table = e & 0x0000fffffffff000ull;
            if (!level) pa = table | (linear & 4095);
        }
        if(guestCpl(s)==3 ? !user : (user && (s.get(0x6804)&0x100000)))return false;
    }
    return m.readPhysical(pa, &byte, 1, true);
}
template<class Memory> bool fetchGuestCode(Memory &m, const VcpuState &s, uint8_t *out, unsigned length) {
    if (!length || length>15) return false;
    uint64_t rip = s.get(0x681e), base = s.get(0x6808);
    bool wide = (s.get(0x2806)&0x400) && (s.get(0x4816)&0x2000);
    if (wide) base = 0;
    else if (rip > s.get(0x4802) || length-1 > s.get(0x4802)-rip) return false;
    if (base > ~0ull-rip || base+rip > ~0ull-(length-1)) return false;
    for (unsigned i=0;i<length;++i) if (!guestCodeByte(m,s,base+rip+i,out[i])) return false;
    return true;
}
}
