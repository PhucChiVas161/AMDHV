#include "io_exit.hpp"
#include "guest_fetch.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace amdhv;
struct Memory {
    std::vector<uint8_t> bytes=std::vector<uint8_t>(0x10000);
    uint64_t missing=~0ull, nx=~0ull;
    bool readPhysical(uint64_t p,void *out,size_t n,bool exec=false) {
        if(p>=bytes.size() || n>bytes.size()-p || p/4096==missing || (exec && p/4096==nx)) return false;
        memcpy(out,bytes.data()+p,n); return true;
    }
    void entry(unsigned off,uint64_t v) {memcpy(bytes.data()+off,&v,8);}
};
int main() {
    for(unsigned bits : {16u,32u,64u}) for(unsigned width : {1u,2u,4u})
    for(bool input : {false,true}) for(bool immediate : {false,true}) {
        uint8_t code[3]; unsigned n=0;
        if(width>1 && ((bits==16 && width==4) || (bits!=16 && width==2))) code[n++]=0x66;
        code[n++]=(immediate?0xe4:0xec) + (input?0:2) + (width==1?0:1);
        if(immediate) code[n++]=0x80;
        uint64_t info=(0x80ull<<16)|(uint64_t(bits/16)<<7)|(width<<4)|input;
        // Address64 encoding is4, not64/16 generalized beyond supported modes.
        IoExit out;
        assert(translateIo(info,code,n,bits,0x80,out));
        assert(out.qualification==((0x80ull<<16)|(immediate?64:0)|(input?8:0)|(width-1)));
        assert(out.instructionInfo==0);
        assert(!translateIo(info^1,code,n,bits,0x80,out));
    }
    IoExit out;
    uint8_t outs[]={0xf3,0x67,0x64,0x6f};
    uint64_t info=(0x3f8ull<<16)|(4<<10)|(2<<7)|(4<<4)|12;
    assert(translateIo(info,outs,4,64,0x3f8,out));
    assert(out.qualification==((0x3f8ull<<16)|3|16|32));
    assert(out.instructionInfo==((1<<7)|(4<<15)));
    uint8_t ins[]={0xf3,0x6c};
    assert(translateIo((0x80ull<<16)|(1<<7)|(1<<4)|13,ins,2,16,0x80,out));
    assert(out.instructionInfo==0);
    uint8_t bad[]={0xf0,0xee}; assert(!translateIo(0x800090,bad,2,16,0x80,out));
    uint8_t truncated[]={0xe6}; assert(!translateIo(0x800090,truncated,1,16,0x80,out));
    uint8_t rex[]={0x48,0xef}; assert(translateIo((0x80ull<<16)|(4<<7)|(4<<4),rex,2,64,0x80,out));
    VcpuState s; s.reset(); Memory m; uint8_t code[2];
    m.bytes[0xfff]=0xe6; m.bytes[0x1000]=0x80; s.set(0x681e,0xfff);
    assert(fetchGuestCode(m,s,code,2) && code[0]==0xe6 && code[1]==0x80);
    m.missing=1; assert(!fetchGuestCode(m,s,code,2)); m.missing=~0ull;
    s.set(0x4802,0xfff); assert(!fetchGuestCode(m,s,code,2)); s.set(0x4802,0xffff);
    s.set(0x6800,0x80000011); s.set(0x6804,0x20); s.set(0x2806,0x500);
    s.set(0x4816,0xa09b); s.set(0x6802,0x2000); s.set(0x681e,0);
    m.entry(0x2000,0x3003); m.entry(0x3000,0x4003); m.entry(0x4000,0x5003); m.entry(0x5000,0x6003);
    m.bytes[0x6000]=0xee;
    assert(fetchGuestCode(m,s,code,1) && code[0]==0xee);
    m.nx=6; assert(!fetchGuestCode(m,s,code,1)); m.nx=~0ull;
    m.missing=5; assert(!fetchGuestCode(m,s,code,1)); m.missing=~0ull;
    m.entry(0x5000,0x8000000000006003ull); assert(!fetchGuestCode(m,s,code,1));
    m.entry(0x4000,0x83); m.bytes[0]=0xec; assert(fetchGuestCode(m,s,code,1) && code[0]==0xec); //2MiB
    m.entry(0x4000,0x2083); assert(!fetchGuestCode(m,s,code,1)); // unaligned huge page
    m.entry(0x3000,0x83); assert(fetchGuestCode(m,s,code,1)); //1GiB
    m.entry(0x2000,0x83); assert(!fetchGuestCode(m,s,code,1)); // illegal PML4 PS
    s.set(0x681e,0x800000000000ull); assert(!fetchGuestCode(m,s,code,1));
    assert(!fetchGuestCode(m,s,code,16));
    s.set(0x681e,0); s.set(0x2806,0xd00);
    m.entry(0x2000,0x3003);m.entry(0x3000,0x4003);m.entry(0x4000,0x5003);m.entry(0x5000,0x6003);
    assert(fetchGuestCode(m,s,code,1) && code[0]==0xee);
    for(unsigned slot : {0x2000u,0x3000u,0x4000u,0x5000u}) {
        uint64_t original=0;memcpy(&original,m.bytes.data()+slot,8);
        m.entry(slot,original|(1ull<<63));assert(!fetchGuestCode(m,s,code,1));
        m.entry(slot,original);assert(fetchGuestCode(m,s,code,1));
    }
    m.entry(0x4000,0x83);assert(fetchGuestCode(m,s,code,1));
    m.entry(0x4000,0x8000000000000083ull);assert(!fetchGuestCode(m,s,code,1));
    m.entry(0x3000,0x83);assert(fetchGuestCode(m,s,code,1));
    m.entry(0x3000,0x8000000000000083ull);assert(!fetchGuestCode(m,s,code,1));
    // High-half virtual code must still walk only owned guest physical pages.
    m.entry(0x2000+256*8,0x3003);m.entry(0x3000,0x4003);m.entry(0x4000,0x5003);m.entry(0x5000,0x6003);
    s.set(0x681e,0xffff800000000000ull);assert(fetchGuestCode(m,s,code,1)&&code[0]==0xee);
    m.missing=6;assert(!fetchGuestCode(m,s,code,1));m.missing=~0ull;
    m.entry(0x5000,0x8000000000006003ull);assert(!fetchGuestCode(m,s,code,1));
    s.set(0x681e,0);s.set(0x6804,0x100020);
    m.entry(0x2000,0x3007);m.entry(0x3000,0x4007);m.entry(0x4000,0x5007);m.entry(0x5000,0x6007);
    assert(!fetchGuestCode(m,s,code,1)); // SMEP user page, all levels U/S1
    for(unsigned slot : {0x2000u,0x3000u,0x4000u,0x5000u}) {
        uint64_t original=0;memcpy(&original,m.bytes.data()+slot,8);
        m.entry(slot,original&~4ull);assert(fetchGuestCode(m,s,code,1));
        m.entry(slot,original);
    }
    m.entry(0x4000,0x87);assert(!fetchGuestCode(m,s,code,1)); // 2MiB U/S
    m.entry(0x3000,0x87);assert(!fetchGuestCode(m,s,code,1)); // 1GiB U/S
    s.set(0x6804,0x20);assert(fetchGuestCode(m,s,code,1));
    s.set(0x802,0x33);s.set(0x4816,0xa0fb);s.set(0x4818,0xc0f3);s.set(0x6804,0x100020);
    assert(fetchGuestCode(m,s,code,1)); // CPL3 executes user pages despite SMEP
    m.entry(0x3000,0x83);assert(!fetchGuestCode(m,s,code,1));
    m.entry(0x3000,0x4007);m.entry(0x4000,0x5007);m.entry(0x5000,0x6007);
    assert(fetchGuestCode(m,s,code,1));
    for(unsigned slot:{0x2000u,0x3000u,0x4000u,0x5000u}){
        uint64_t original=0;memcpy(&original,m.bytes.data()+slot,8);
        m.entry(slot,original&~4ull);assert(!fetchGuestCode(m,s,code,1));m.entry(slot,original);
    }
    puts("PASS: IO widths/directions/ports/prefixes/string metadata; guest fetch4K/2M/1G, high canonical, holes/NX/bounds");
}
