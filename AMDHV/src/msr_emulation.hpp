#pragma once
#include "vcpu_state.hpp"
namespace amdhv {
enum class MsrAction { Exit, Reject, Handled };
// Pure policy/state transition; the caller supplies a clock sample and hardware-saved aliases.
// No host MSR access is performed by this function.
inline MsrAction emulateManagedMsr(VcpuState &s, uint32_t address, bool write, uint64_t &value, uint64_t now) {
    unsigned id = managedMsrId(address);
    if (!id || !(s.managedMask & (1ull << id)) ||
        !((write ? s.msrWriteMask : s.msrReadMask) & (1ull << id))) return MsrAction::Exit;
    if (id == TscMsrId && !s.relativeTsc) return MsrAction::Reject;
    if (write) {
        if (!validManagedMsr(id, value)) return MsrAction::Reject;
        if (id == TscMsrId) s.tscOffset = value - now;
        else s.setMsrValue(id, value);
    } else value = id == TscMsrId ? now + s.tscOffset : s.msrValue(id);
    return MsrAction::Handled;
}
}
