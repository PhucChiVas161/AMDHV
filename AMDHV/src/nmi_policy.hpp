#pragma once
#include "vcpu_state.hpp"
#include "exception_policy.hpp"
namespace amdhv {
constexpr uint64_t VirtualNmiBlocking=1ull<<12;
constexpr uint64_t VirtualNmiEnable=1ull<<26;
constexpr uint64_t VirtualGifEnable=1ull<<25;
constexpr uint64_t VirtualGifOpen=1ull<<9;
constexpr uint64_t NmiEntry=0x80000202ull;
inline bool validNativeNmiEntry(const VcpuState &s,bool fast) {
    // Explicit delivery only; no hidden pending NMI or NMI-window control.
    return nativeFaultMode(s,fast) && s.get(0x4016)==NmiEntry && !s.get(0x4824);
}
inline bool needsVirtualNmi(const VcpuState &s) {
    return (s.get(0x4824)&8) || s.get(0x4016)==NmiEntry;
}
}
