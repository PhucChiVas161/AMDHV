#pragma once
#include "vcpu_state.hpp"
namespace amdhv {
inline bool validDisabledDr7(uint64_t value) { return !(value & ~0xffff0700ull); }
inline bool validGuestDr6(uint64_t value) { return !(value & ~0xffffefffull); }
struct DebugMove { unsigned dr=0, gpr=0; bool write=false; };
inline bool decodeDebugMove(const uint8_t *code,unsigned length,bool wide,uint64_t exit,DebugMove &d) {
    if (exit<0x20 || exit>0x3f || !length || length>15) return false;
    unsigned i=0; uint8_t rex=0;
    while(i<length) {
        uint8_t b=code[i];
        if(b==0x66 || b==0x67 || b==0x26 || b==0x2e || b==0x36 || b==0x3e || b==0x64 || b==0x65) {rex=0;++i;continue;}
        if(wide && b>=0x40 && b<=0x4f) {rex=b;++i;continue;}
        break;
    }
    if(length-i!=3 || code[i]!=0x0f || (code[i+1]!=0x21 && code[i+1]!=0x23) ||
       (code[i+2]&0xc0)!=0xc0 || (rex&4)) return false;
    d.dr=(code[i+2]>>3)&7; d.gpr=(code[i+2]&7)|((rex&1)?8:0); d.write=code[i+1]==0x23;
    return d.dr==(exit&15) && d.write==bool(exit&16) && d.dr!=4 && d.dr!=5;
}
inline unsigned debugGprIndex(unsigned reg) {
    constexpr unsigned ids[]={2,3,4,5,8,9,6,7,10,11,12,13,14,15,16,17};
    return ids[reg&15];
}
inline bool emulateDebugMove(VcpuState &s,const DebugMove &d,bool wide) {
    const unsigned id=debugGprIndex(d.gpr);
    uint64_t value=d.gpr==4?s.get(0x681c):s.registers[id];
    if(!wide) value=uint32_t(value);
    if(d.write) {
        if(d.dr<4) {
            if(!validManagedMsr(6,value)) return false;
            s.registers[41+d.dr]=value;
        } else if(d.dr==6) {
            if(!validGuestDr6(value)) return false;
            s.registers[47]=(value&0xe00f)|0xffff0ff0ull;
        } else if(d.dr==7) {
            if(!validDisabledDr7(value)) return false;
            s.set(0x681a,value|0x400);
        } else return false;
    } else {
        value=d.dr<4?s.registers[41+d.dr]:d.dr==6?((s.registers[47]&0xe00f)|0xffff0ff0ull):(s.get(0x681a)|0x400);
        if(!wide) value=uint32_t(value);
        if(d.gpr==4) s.set(0x681c,value); else s.registers[id]=value;
    }
    return true;
}
}
