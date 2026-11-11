#pragma once
#include <stdint.h>
#include "guest_address.hpp"
namespace amdhv {
constexpr uint64_t PilotFlags = 0x200ed7; // arithmetic/DF/IF/ID + fixed bit1; no guest TF/IOPL
// PUSHF/POPF have no branch. A temporary execution breakpoint at NRIP bounds
// one real instruction while TF is clear, so the guest never sees our debug TF.
inline bool flagsStepTarget(uint64_t rip, uint64_t next, uint64_t csBase,
                            bool code64, uint64_t limit, uint64_t &linear) {
    if (next <= rip || next-rip > 15) return false;
    if (!code64 && (next > limit || csBase > ~0ull-next)) return false;
    linear = code64 ? next : csBase+next;
    if (!code64 && linear > 0xffffffffull) return false; // reject 32-bit linear wrap
    // Zero is the assembly 'no breakpoint' sentinel. Check both ends so an
    // instruction cannot cross the noncanonical hole or wrap the address space.
    return linear && canonical48(linear) && (!code64 || canonical48(rip));
}
inline bool flagsStepComplete(uint64_t exit, uint64_t rip, uint64_t next, uint64_t dr6) {
    return exit == 0x41 && rip == next && (dr6 & 1);
}
// Some instruction/fetch intercepts can win at the same next-instruction
// boundary. They are still bounded: the flags instruction completed and the
// following instruction has not executed. Forward only known pre-execution exits.
inline bool flagsStepBoundary(uint64_t exit, uint64_t rip, uint64_t next, uint64_t dr6) {
    if (rip != next) return false;
    if (exit == 0x41) return flagsStepComplete(exit,rip,next,dr6);
    return exit <= 0x3f || exit == 0x72 || exit == 0x77 || exit == 0x78 || exit == 0x79 ||
           exit == 0x7b || exit == 0x7c || exit == 0x400;
}
}
