#pragma once
#include <stdint.h>
#include "xstate_policy.hpp"
namespace amdhv {
// NXE/SCE are orthogonal to paging mode. SCE is separately gated to prepared
// LME/active IA-32e in the host-IRQ runner: SYSCALL/SFMASK can clear debug TF.
// Preserve every other EFER bit so SVME/reserved bits still fail validation.
constexpr uint64_t pagingEfer(uint64_t efer) { return efer & ~0x801ull; }
// PE/MP/ET/NE plus WP/AM. EM/TS, caching controls and mode transitions remain gated.
constexpr bool validLegacyCr0(uint64_t value) { return (value & ~0x50033ull) == 0; }
// PAE may be prepared with paging off. OSFXSR permits the switched x87/SSE subset;
// DE is safe with every MOV DR intercepted and guest DR7 breakpoints disabled.
// DR4/DR5 accesses still fail closed; #UD synthesis is not implemented.
// Extra native bits are restricted to fast code64 by the caller. OSXSAVE also
// requires the bounded extended switch. PCID/LA57 and protection keys stay gated.
constexpr uint64_t NativeCr4Mask=0x310800; // FSGSBASE, SMEP, SMAP, UMIP
constexpr uint64_t supportedNativeCr4(uint32_t ebx,uint32_t ecx) {
    return ((ebx&1)?0x10000ull:0) | ((ebx&(1u<<7))?0x100000ull:0) |
        ((ebx&(1u<<20))?0x200000ull:0) | ((ecx&(1u<<2))?0x800ull:0);
}
constexpr bool validLegacyCr4(uint64_t value, uint64_t mask, uint64_t shadow,uint64_t native=0) {
    return !(value & ~(0x26f8ull|(native&(NativeCr4Mask|NativeXsaveCr4|NativePkeCr4)))) && (!(value & 0x2000) || ((mask & 0x2000) && !(shadow & 0x2000)));
}
constexpr bool validPilotCr3(uint64_t value) { return !(value & ~0x0000fffffffff000ull); }
}
