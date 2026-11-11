#pragma once
#include "vcpu_state.hpp"
#include "cr_policy.hpp"
namespace amdhv {
inline uint64_t guestCr(const VcpuState &s, unsigned cr) {
    if (cr == 2) return s.registers[38];
    if (cr == 3) return s.get(0x6802);
    const unsigned field = cr == 0 ? 0x6800 : 0x6804;
    const unsigned mask = cr == 0 ? 0x6000 : 0x6002;
    const unsigned shadow = cr == 0 ? 0x6004 : 0x6006;
    return ((s.get(field) | (cr == 0 ? 0x10ull : 0ull)) & ~s.get(mask)) | (s.get(shadow) & s.get(mask));
}
enum class CrAction { Reject, Handled, Exit };
inline bool validCrExecutionMode(uint64_t cr0, uint64_t cr4, uint64_t efer) {
    efer = pagingEfer(efer);
    if (!validLegacyCr0(cr0 & ~0x80000000ull)) return false;
    if (cr0 & 0x80000000ull) return (cr0 & 1) && (cr4 & 0x20) && efer == 0x500;
    return efer == 0 || (efer == 0x100 && (cr0 & 1) && (cr4 & 0x20));
}
// No paging-mode transitions. Masks are checked before merging guest-owned bits.
inline CrAction emulateCr(VcpuState &s, unsigned cr, bool write, uint64_t &value,uint64_t nativeCr4=0) {
    // CR2 is a guest virtual address register, not a host pointer or table root.
    // Legacy writes clear the high half; IA-32e preserves all 64 bits.
    if(cr==2) {
        if(write) s.registers[38]=(s.get(0x2806)&0x400)?value:uint32_t(value);
        else value=s.registers[38];
        return CrAction::Handled;
    }
    if(cr==8) {
        if(pagingEfer(s.get(0x2806))!=0x500 || !(s.get(0x4816)&0x2000))return CrAction::Reject;
        if(s.get(0x4002)&(write?0x80000ull:0x100000ull))return CrAction::Exit;
        if(write) {if(value>15)return CrAction::Reject;s.registers[49]=value<<4;}
        else value=s.registers[49]>>4;
        return CrAction::Handled;
    }
    if (cr != 0 && cr != 3 && cr != 4) return CrAction::Reject;
    if (!write) { value = guestCr(s, cr); return CrAction::Handled; }
    if (!validCrExecutionMode(s.get(0x6800), s.get(0x6804), s.get(0x2806))) return CrAction::Reject;
    if (cr == 3) {
        // Every subsequent VMRUN flushes all ASIDs, including global translations.
        if (!validPilotCr3(value)) return CrAction::Reject;
        s.set(0x6802, value); return CrAction::Handled;
    }
    const unsigned field = cr == 0 ? 0x6800 : 0x6804;
    uint64_t mask = s.get(cr == 0 ? 0x6000 : 0x6002);
    uint64_t shadow = s.get(cr == 0 ? 0x6004 : 0x6006);
    if ((value ^ shadow) & mask) return CrAction::Exit;
    uint64_t old = guestCr(s, cr);
    if (value == old) return CrAction::Handled;
    uint64_t merged = (s.get(field) & mask) | (value & ~mask);
    if (cr == 0) {
        if (((merged ^ s.get(field)) & 0x80000000ull) ||
            ((s.get(field) & 1) && !(merged & 1)) ||
            !validCrExecutionMode(merged, s.get(0x6804), s.get(0x2806))) return CrAction::Reject;
        merged |= 0x10; // architectural ET is fixed to one; VMCS field itself may contain zero
    } else if (!validLegacyCr4(merged, mask, shadow,nativeCr4) ||
               !validCrExecutionMode(s.get(0x6800), merged, s.get(0x2806))) return CrAction::Reject;
    s.set(field, merged);
    return CrAction::Handled;
}
inline bool decodeCrExit(uint64_t exit, uint64_t info, uint64_t rip, uint64_t nrip,
                         unsigned &cr, unsigned &reg, bool &write, unsigned &length) {
    if (exit > 0x1f || !(info & (1ull << 63)) || nrip <= rip || nrip-rip > 15) return false;
    cr = static_cast<unsigned>(exit & 15); reg = static_cast<unsigned>(info & 15);
    write = (exit & 16) != 0; length = static_cast<unsigned>(nrip-rip);
    return cr == 0 || cr == 2 || cr == 3 || cr == 4 || cr == 8;
}
constexpr uint64_t crQualification(unsigned cr, unsigned reg, bool write) {
    return cr | (write ? 0ull : 16ull) | (uint64_t(reg) << 8);
}
}
