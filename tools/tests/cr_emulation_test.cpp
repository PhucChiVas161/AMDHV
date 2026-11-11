#include "cr_emulation.hpp"
#include "vcpu_mode.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>
using namespace amdhv;
int main() {
    VcpuState s; s.reset(); s.set(0x6800,0x30); s.set(0x6804,0x2000);
    s.set(0x6000,0xe0000020); s.set(0x6004,0x60000010);
    s.set(0x6002,0x2000); s.set(0x6006,0); s.set(0x4002,0x200080);
    s.set(0x802,0xf000); s.set(0x6808,0xffff0000); s.set(0x681e,0xfff0);
    assert(validPilotMode(s) && validPilotControls(s));
    uint64_t value=0;
    assert(emulateCr(s,0,false,value)==CrAction::Handled && value==0x60000010);
    assert(emulateCr(s,0,true,value)==CrAction::Handled);
    value=0; assert(emulateCr(s,4,false,value)==CrAction::Handled && value==0);
    assert(emulateCr(s,4,true,value)==CrAction::Handled);
    value=0x10; assert(emulateCr(s,0,true,value)==CrAction::Exit);
    value=0x60000011; assert(emulateCr(s,0,true,value)==CrAction::Handled);
    assert(s.get(0x6800)==0x31 && validPilotMode(s) && validPilotControls(s));
    value=0x60000010; assert(emulateCr(s,0,true,value)==CrAction::Reject);
    s.set(0x6800,0x30);
    value=0x60000014; assert(emulateCr(s,0,true,value)==CrAction::Reject);
    assert(s.get(0x6800)==0x30 && s.get(0x6804)==0x2000);
    assert(emulateCr(s,8,false,value)==CrAction::Reject);
    s.set(0x6002,0); assert(!validPilotMode(s)); s.set(0x6002,0x2000);
    s.set(0x6006,0x2000); assert(!validPilotMode(s)); s.set(0x6006,0);
    s.set(0x401c,1); assert(!validPilotControls(s)); s.set(0x401c,0);
    s.set(0x4012,0x200); assert(!validPilotControls(s)); s.set(0x4012,0); s.set(0x2806,0); s.set(0x6800,0x30);
    s.set(0x4002,0x200081); assert(!validPilotControls(s));
    s.reset(); s.set(0x6800,0x23); s.set(0x6804,0x2000);
    s.set(0x6000,0xe0000020); s.set(0x6004,0x40000023); s.set(0x6002,0x2000);
    assert(validPilotMode(s)); assert(guestCr(s,0)==0x40000033);
    value=0x640; assert(emulateCr(s,4,true,value)==CrAction::Handled && s.get(0x6804)==0x2640);
    value=0x660; assert(emulateCr(s,4,true,value)==CrAction::Handled && s.get(0x6804)==0x2660);
    assert(guestCr(s,4)==0x660 && validPilotMode(s));
    value=0x800000; assert(emulateCr(s,3,true,value)==CrAction::Handled);
    assert(guestCr(s,3)==0x800000 && validPilotMode(s));
    value=0x800001; assert(emulateCr(s,3,true,value)==CrAction::Reject && s.get(0x6802)==0x800000);
    value=1ull<<48; assert(emulateCr(s,3,true,value)==CrAction::Reject);
    value=0x40000; assert(emulateCr(s,4,true,value)==CrAction::Reject && s.get(0x6804)==0x2660);
    value=0xc0000033; assert(emulateCr(s,0,true,value)==CrAction::Exit && s.get(0x6800)==0x23);
    s.set(0x6000,0); value=0x80000033; assert(emulateCr(s,0,true,value)==CrAction::Reject);
    // Active long mode: changing protection bits and table roots must not be
    // confused with entering/leaving paging. Rejections never commit state.
    s.reset(); s.set(0x6800,0x80000033); s.set(0x6804,0x660); s.set(0x2806,0x500);
    value=0x80050033; assert(emulateCr(s,0,true,value)==CrAction::Handled && s.get(0x6800)==value);
    value=0x6f0; assert(emulateCr(s,4,true,value)==CrAction::Handled && s.get(0x6804)==value);
    value=0x12345000; assert(emulateCr(s,3,true,value)==CrAction::Handled && s.get(0x6802)==value);
    for(uint64_t bad : {0x12345001ull,1ull<<48,1ull<<63}) {
        value=bad; assert(emulateCr(s,3,true,value)==CrAction::Reject && s.get(0x6802)==0x12345000);
    }
    for(uint64_t bad : {0x50033ull,0x80050032ull,0x80050037ull,0xc0050033ull,1ull<<32}) {
        value=bad; assert(emulateCr(s,0,true,value)==CrAction::Reject && s.get(0x6800)==0x80050033);
    }
    for(uint64_t bad : {0x6d0ull,0x406f0ull,0x206f0ull,1ull<<32}) {
        value=bad; assert(emulateCr(s,4,true,value)==CrAction::Reject && s.get(0x6804)==0x6f0);
    }
    // Masked writes still exit to Apple before private changes.
    s.set(0x6000,0x10000); s.set(0x6004,0x10000);
    value=0x80040033; assert(emulateCr(s,0,true,value)==CrAction::Exit && s.get(0x6800)==0x80050033);
    s.set(0x6002,0x80); s.set(0x6006,0x80);
    value=0x670; assert(emulateCr(s,4,true,value)==CrAction::Exit && s.get(0x6804)==0x6f0);
    s.set(0x2806,0x100); value=0x1000; assert(emulateCr(s,3,true,value)==CrAction::Reject);
    s.set(0x6800,0x50033); assert(emulateCr(s,3,true,value)==CrAction::Handled); // LME armed, PG0
    s.reset(); s.set(0x6800,0x80010033); s.set(0x6804,0x660); s.set(0x2806,0xd00);
    value=0x4000; assert(emulateCr(s,3,true,value)==CrAction::Handled && s.get(0x2806)==0xd00);
    value=0x80050033; assert(emulateCr(s,0,true,value)==CrAction::Handled);
    value=0x6f0; assert(emulateCr(s,4,true,value)==CrAction::Handled);
    value=0x6d0; assert(emulateCr(s,4,true,value)==CrAction::Reject);
    s.set(0x2806,0xd01); value=0x1000; assert(emulateCr(s,3,true,value)==CrAction::Handled);
    s.set(0x2806,0xd04); value=0x1000; assert(emulateCr(s,3,true,value)==CrAction::Reject);
    // Native EFI enables CR4.DE after saving DR state. Preserve it across
    // re-entry and reads; CR masks still exit before changing private state.
    s.reset(); s.set(0x6800,0x80010033); s.set(0x6804,0x660); s.set(0x2806,0xd00);
    for(unsigned i=0;i<6;++i) {
        s.set(0x4800+i*2,0xffffffff); s.set(0x4814+i*2,i==1?0xa09b:0xc093);
    }
    value=0x668; assert(emulateCr(s,4,true,value)==CrAction::Handled);
    assert(s.get(0x6804)==0x668 && validPilotMode(s));
    value=0; assert(emulateCr(s,4,false,value)==CrAction::Handled && value==0x668);
    s.set(0x6002,8); s.set(0x6006,8); value=0x660;
    assert(emulateCr(s,4,true,value)==CrAction::Exit && s.get(0x6804)==0x668);
    s.set(0x6002,0); value=0x660; assert(emulateCr(s,4,true,value)==CrAction::Handled);
    assert(s.get(0x6804)==0x660 && validPilotMode(s));
    for(uint64_t bit : {0x800ull,0x10000ull,0x100000ull,0x200000ull}) {
        s.set(0x6002,0);value=0x660|bit;
        assert(emulateCr(s,4,true,value)==CrAction::Reject&&s.get(0x6804)==0x660);
        assert(emulateCr(s,4,true,value,NativeCr4Mask)==CrAction::Handled&&s.get(0x6804)==value);
        value=0x660;assert(emulateCr(s,4,true,value,NativeCr4Mask)==CrAction::Handled);
        s.set(0x6002,bit);s.set(0x6006,0);value=0x660|bit;
        assert(emulateCr(s,4,true,value,NativeCr4Mask)==CrAction::Exit&&s.get(0x6804)==0x660);
    }
    s.set(0x6002,0);value=0x40660;
    assert(emulateCr(s,4,true,value,NativeCr4Mask)==CrAction::Reject&&s.get(0x6804)==0x660);
    assert(emulateCr(s,4,true,value,NativeCr4Mask|NativeXsaveCr4)==CrAction::Handled&&s.get(0x6804)==0x40660);
    value=0x660;assert(emulateCr(s,4,true,value,NativeCr4Mask|NativeXsaveCr4)==CrAction::Handled);
    value=0x400660;
    assert(emulateCr(s,4,true,value,NativeCr4Mask|NativeXsaveCr4|NativePkeCr4)==CrAction::Handled);
    assert(s.get(0x6804)==0x400660 && guestCr(s,4)==0x400660);
    value=0x660;assert(emulateCr(s,4,true,value,NativeCr4Mask|NativePkeCr4)==CrAction::Handled);
    s.set(0x6002,NativePkeCr4);s.set(0x6006,0);value=0x400660;
    assert(emulateCr(s,4,true,value,NativeCr4Mask|NativePkeCr4)==CrAction::Exit&&s.get(0x6804)==0x660);
    s.set(0x6002,0);
    unsigned cr=0,reg=0,len=0; bool write=false;
    assert(decodeCrExit(0x14,(1ull<<63)|9,0x100,0x104,cr,reg,write,len));
    assert(cr==4 && reg==9 && write && len==4 && crQualification(cr,reg,write)==0x904);
    assert(!decodeCrExit(0x14,9,0x100,0x104,cr,reg,write,len));
    assert(!decodeCrExit(0x11,1ull<<63,0x100,0x104,cr,reg,write,len));
    assert(!decodeCrExit(0,1ull<<63,0x100,0x100,cr,reg,write,len));
    assert(!decodeCrExit(0,1ull<<63,0x100,0x110,cr,reg,write,len));
    s.reset();s.set(0x2806,0x500);s.set(0x4816,0xa09b);
    value=9;assert(emulateCr(s,8,true,value)==CrAction::Handled&&s.registers[49]==0x90);
    value=0;assert(emulateCr(s,8,false,value)==CrAction::Handled&&value==9);
    value=16;assert(emulateCr(s,8,true,value)==CrAction::Reject&&s.registers[49]==0x90);
    s.set(0x4002,0x100000);assert(emulateCr(s,8,false,value)==CrAction::Exit);
    s.set(0x4002,0x80000);assert(emulateCr(s,8,true,value)==CrAction::Exit&&s.registers[49]==0x90);
    assert(decodeCrExit(8,1ull<<63,0x8000,0x8004,cr,reg,write,len)&&cr==8&&!write);
    // CR2 accepts address bits without treating the value as a host address.
    value=0xfedcba9876543210ull;
    assert(emulateCr(s,2,true,value)==CrAction::Handled && s.registers[38]==value);
    value=0; assert(emulateCr(s,2,false,value)==CrAction::Handled && value==0xfedcba9876543210ull);
    assert(decodeCrExit(2,(1ull<<63)|9,0x8000,0x8003,cr,reg,write,len)&&cr==2&&!write&&reg==9);
    assert(decodeCrExit(0x12,1ull<<63,0x8000,0x8003,cr,reg,write,len)&&cr==2&&write);
    s.set(0x2806,0); value=0xfedcba9876543210ull;
    assert(emulateCr(s,2,true,value)==CrAction::Handled && s.registers[38]==0x76543210);
    puts("PASS: native reset validation, CR mask/shadow emulation, fail-closed transitions and exit decode");
}
