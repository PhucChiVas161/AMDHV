#include "msr_emulation.hpp"
#include "speculation_policy.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>
using namespace amdhv;
int main() {
    VcpuState s; s.reset(); s.relativeTsc = true;
    // Exercise the actual handler for every descriptor, permissions and alias, not a mock copy.
    for (unsigned id = 1; id <= ManagedMsrCount; ++id) {
        uint32_t address = managedMsrAddress(id);
        uint64_t bit = 1ull << id, value = 0x1000 + id;
        s.managedMask = s.msrReadMask = s.msrWriteMask = 0;
        assert(emulateManagedMsr(s,address,true,value,100) == MsrAction::Exit);
        s.managedMask = bit; s.msrReadMask = bit;
        assert(emulateManagedMsr(s,address,true,value,100) == MsrAction::Exit);
        s.msrWriteMask = bit;
        assert(emulateManagedMsr(s,address,true,value,100) == MsrAction::Handled);
        uint64_t got = 0;
        assert(emulateManagedMsr(s,address,false,got,125) == MsrAction::Handled);
        assert(got == value + (id == TscMsrId ? 25 : 0));
        if (managedMsrVmcs(id)) assert(s.get(managedMsrVmcs(id)) == value);
        s.msrReadMask = 0;
        assert(emulateManagedMsr(s,address,false,got,125) == MsrAction::Exit);
    }
    s.managedMask = s.msrReadMask = s.msrWriteMask = ~0ull;
    uint64_t value = 99;
    assert(emulateManagedMsr(s,0xc0010117,true,value,0) == MsrAction::Exit); // HSAVE isolated
    for (unsigned id = 1; id <= ManagedMsrCount; ++id) {
        if (id == 1 || id == TscMsrId) continue;
        value = id == 4 || id == 8 ? (1ull << 32) : 0x800000000000ull;
        auto prior = s.msrValue(id);
        assert(emulateManagedMsr(s,managedMsrAddress(id),true,value,0) == MsrAction::Reject);
        assert(s.msrValue(id) == prior);
    }
    value = 10;
    assert(emulateManagedMsr(s,0x10,true,value,100) == MsrAction::Handled);
    assert(s.tscOffset == uint64_t(-90));
    assert(emulateManagedMsr(s,0x10,false,value,120) == MsrAction::Handled && value == 30);
    s.relativeTsc = false;
    assert(emulateManagedMsr(s,0x10,true,value,100) == MsrAction::Reject);
    constexpr auto caps=speculationCaps((1u<<12)|(1u<<14)|(1u<<15)|(1u<<24)|(1u<<28),1u<<20,1u<<27);
    static_assert(caps.controls==0x87 && caps.commands==0x81 && caps.virtualControls);
    for(uint64_t bits=0;bits<256;++bits) {
        assert(speculationAction(caps,0x48,true,bits)==((bits&~0x87ull)?SpeculationAction::Fault:SpeculationAction::WriteControl));
        assert(speculationAction(caps,0x49,true,bits)==((bits&~0x81ull)?SpeculationAction::Fault:SpeculationAction::Command));
    }
    assert(speculationAction(caps,0x49,true,1ull<<32)==SpeculationAction::Fault);
    assert(speculationAction(caps,0x49,false,0)==SpeculationAction::Fault);
    assert(speculationAction(caps,0x48,false,0)==SpeculationAction::ReadControl);
    assert(speculationAction(caps,0x10b,true,1)==SpeculationAction::Fault);
    auto flush=speculationCaps(0,0,0,1u<<28);
    assert(speculationAction(flush,0x10b,true,0)==SpeculationAction::Command);
    assert(speculationAction(flush,0x10b,true,1)==SpeculationAction::Command);
    assert(speculationAction(flush,0x10b,true,2)==SpeculationAction::Fault);
    assert(speculationAction(flush,0x10b,false,0)==SpeculationAction::Fault);
    assert(speculationAction(flush,0xc001011f,true,4)==SpeculationAction::Forward);
    for(unsigned bit:{12u,14u,15u,24u,28u}) {
        auto c=speculationCaps(1u<<bit,1u<<20,0);
        assert(c.commands==(bit==12?1u:0u));
        assert(c.controls==(bit==14?1u:bit==15?2u:bit==24?4u:bit==28?128u:0u));
    }
    auto none=speculationCaps(0,0,0);
    assert(speculationAction(none,0x49,true,0)==SpeculationAction::Fault);
    assert(speculationAction(none,0x48,true,0)==SpeculationAction::Forward);
    auto noVirtual=speculationCaps(~0u,0,~0u);
    assert(speculationAction(noVirtual,0x48,true,1)==SpeculationAction::Forward);
    s.specCtrl=0x87;s.reset();assert(s.specCtrl==0);
    puts("PASS: managed MSRs/TSC/aliases; speculation capability masks, commands, reserved bits, virtualization gate and reset isolation");
}
