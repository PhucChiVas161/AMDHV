#include "debug_emulation.hpp"
#include <cassert>
#include <cstdio>
using namespace amdhv;
int main() {
    DebugMove d;
    const uint8_t rd[]={0x0f,0x21,0xc0}, wr[]={0x41,0x0f,0x23,0xc1};
    assert(decodeDebugMove(rd,3,true,0x20,d)&&d.dr==0&&d.gpr==0&&!d.write);
    assert(!decodeDebugMove(rd,2,true,0x20,d));
    assert(!decodeDebugMove(rd,3,true,0x21,d));
    assert(!decodeDebugMove(rd,3,true,0x30,d));
    assert(decodeDebugMove(wr,4,true,0x30,d)&&d.gpr==9&&d.write);
    assert(!decodeDebugMove(wr,4,false,0x30,d));
    const uint8_t bad[]={0x44,0x0f,0x21,0xc0};assert(!decodeDebugMove(bad,4,true,0x20,d));
    const uint8_t mem[]={0x0f,0x21,0};assert(!decodeDebugMove(mem,3,true,0x20,d));
    VcpuState s;s.reset();d={0,0,true};s.registers[2]=0x123456789ull;
    assert(emulateDebugMove(s,d,true)&&s.registers[41]==0x123456789ull);
    d={0,1,false};assert(emulateDebugMove(s,d,false)&&s.registers[3]==0x23456789);
    d={7,0,true};s.registers[2]=0x401;assert(!emulateDebugMove(s,d,true)&&s.get(0x681a)==0x400);
    s.registers[2]=0;assert(emulateDebugMove(s,d,true)&&s.get(0x681a)==0x400);
    d={6,0,true};assert(emulateDebugMove(s,d,true)&&s.registers[47]==0xffff0ff0);
    s.registers[2]=1ull<<32;assert(!emulateDebugMove(s,d,true)&&s.registers[47]==0xffff0ff0);
    d={0,4,false};assert(emulateDebugMove(s,d,true)&&s.get(0x681c)==0x123456789ull);
    d={0,0,true};s.registers[2]=0x800000000000ull;assert(!emulateDebugMove(s,d,true));
    puts("PASS: MOV DR decode, disabled debug state, width, reserved bits and rejection without mutation");
}
