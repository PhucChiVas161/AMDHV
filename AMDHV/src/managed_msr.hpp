#pragma once
#include <stdint.h>
#include "guest_address.hpp"
namespace amdhv {
// Framework IDs from the installed lookup; offsets are AMD VMCB save-area fields.
struct ManagedMsr { uint32_t address; unsigned frameworkId, offset, vmcs; };
constexpr ManagedMsr ManagedMsrs[] = {
    {0xc0000081, 1, 0x600, 0}, {0xc0000082, 2, 0x608, 0},
    {0xc0000083, 3, 0x610, 0}, {0xc0000084, 4, 0x618, 0},
    {0xc0000102, 5, 0x620, 0}, {0xc0000100, 148, 0x448, 0x680e},
    {0xc0000101, 149, 0x458, 0x6810},
    {0x174, 145, 0x628, 0x482a}, {0x175, 146, 0x630, 0x6824},
    {0x176, 147, 0x638, 0x6826},
    {0x10, 144, 0, 0}, // emulated TSC: never loaded into a host MSR or VMCB save slot
};
constexpr unsigned TscMsrId = 11;
constexpr unsigned ManagedMsrCount = sizeof(ManagedMsrs) / sizeof(*ManagedMsrs);
constexpr unsigned managedMsrId(uint32_t address) {
    for (unsigned i = 0; i < ManagedMsrCount; ++i) if (ManagedMsrs[i].address == address) return i + 1;
    return 0;
}
constexpr unsigned compactMsrId(uint64_t frameworkId) {
    for (unsigned i = 0; i < ManagedMsrCount; ++i) if (ManagedMsrs[i].frameworkId == frameworkId) return i + 1;
    return 0;
}
constexpr uint32_t managedMsrAddress(unsigned id) {
    return id && id <= ManagedMsrCount ? ManagedMsrs[id - 1].address : 0;
}
constexpr unsigned managedMsrOffset(unsigned id) {
    return id && id <= ManagedMsrCount ? ManagedMsrs[id - 1].offset : 0;
}
constexpr unsigned managedMsrVmcs(unsigned id) {
    return id && id <= ManagedMsrCount ? ManagedMsrs[id - 1].vmcs : 0;
}
constexpr bool validManagedMsr(unsigned id, uint64_t value) {
    if (id < 1 || id > ManagedMsrCount) return false;
    if (id == 1 || id == TscMsrId) return true;
    if (id == 4 || id == 8) return value <= 0xffffffffull;
    return canonical48(value);
}
}
