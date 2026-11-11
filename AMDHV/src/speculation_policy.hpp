#pragma once
#include <stdint.h>
namespace amdhv {
// Raw AMD CPUID, not VMM-advertised features. Stateful controls require the
// hardware guest/host OR model; never install a guest value in host SPEC_CTRL.
struct SpeculationCaps {
    uint64_t controls, commands;
    bool virtualControls;
    bool l1dFlush;
};
constexpr SpeculationCaps speculationCaps(uint32_t ebx8,uint32_t edxA,uint32_t eax21,uint32_t edx7=0) {
    return {((ebx8&(1u<<14))?1ull:0)|((ebx8&(1u<<15))?2ull:0)|
            ((ebx8&(1u<<24))?4ull:0)|((ebx8&(1u<<28))?0x80ull:0),
            ((ebx8&(1u<<12))?1ull:0)|((eax21&(1u<<27))?0x80ull:0),
            bool(edxA&(1u<<20)),bool(edx7&(1u<<28))};
}
enum class SpeculationAction { Forward, Fault, ReadControl, WriteControl, Command };
constexpr SpeculationAction speculationAction(SpeculationCaps caps,uint32_t address,bool write,uint64_t value) {
    if(address==0x10b) {
        if(!caps.l1dFlush || !write || (value&~1ull))return SpeculationAction::Fault;
        return SpeculationAction::Command;
    }
    if(address==0x49) {
        if(!caps.commands || !write || (value&~caps.commands))return SpeculationAction::Fault;
        return SpeculationAction::Command;
    }
    if(address!=0x48)return SpeculationAction::Forward;
    if(!caps.virtualControls || !caps.controls)return SpeculationAction::Forward;
    if(write && (value&~caps.controls))return SpeculationAction::Fault;
    return write?SpeculationAction::WriteControl:SpeculationAction::ReadControl;
}
}
