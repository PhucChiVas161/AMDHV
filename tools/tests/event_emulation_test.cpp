#include "event_emulation.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace amdhv;
struct Memory {
    std::vector<uint8_t> bytes=std::vector<uint8_t>(2*1024*1024);
    bool writable[512],readable[512];
    Memory(){for(unsigned i=0;i<512;++i)writable[i]=readable[i]=true;}
    bool readPhysical(uint64_t p,void *out,size_t n) {
        if(p>=bytes.size() || n>bytes.size()-p)return false;
        for(size_t i=0;i<n;++i)if(!readable[(p+i)/4096])return false;
        memcpy(out,bytes.data()+p,n);return true;
    }
    bool writePhysical(uint64_t p,const void *in,size_t n,bool check=false) {
        if(p>=bytes.size() || n>bytes.size()-p)return false;
        for(size_t i=0;i<n;++i)if(!writable[(p+i)/4096])return false;
        if(!check)memcpy(bytes.data()+p,in,n);return true;
    }
    void q(uint64_t p,uint64_t v){memcpy(bytes.data()+p,&v,8);}
    uint64_t q(uint64_t p){uint64_t v;memcpy(&v,bytes.data()+p,8);return v;}
};
static void setup(Memory &m,VcpuState &s) {
    memset(m.bytes.data(),0,m.bytes.size());
    s.reset();s.set(0x6800,0x80010033);s.set(0x6802,0x1000);s.set(0x6804,0x20);s.set(0x2806,0xd00);
    s.set(0x4816,0xa09b);s.set(0x802,8);s.set(0x804,16);s.set(0x681e,0x10000);s.set(0x681c,0x7007);
    s.set(0x6820,0x246);s.set(0x6816,0x4000);s.set(0x4810,23);s.set(0x6818,0x5000);s.set(0x4812,4095);
    m.q(0x1000,0x2003);m.q(0x2000,0x3003);m.q(0x3000,0x83);m.q(0x4008,0x00af9b000000ffffull);
    auto g=m.bytes.data()+0x5400;g[0]=0;g[1]=0x10;g[2]=8;g[5]=0x8e;g[6]=1;
    s.set(0x4016,0x80000040);
}
int main() {
    Memory m;VcpuState s;setup(m,s);
    assert(emulateExternalEntry(m,s));
    assert(s.get(0x681c)==0x6fd8 && s.get(0x681e)==0x11000 && s.get(0x6820)==0x46 && !s.get(0x4016));
    assert(m.q(0x6fd8)==0x10000 && m.q(0x6fe0)==8 && m.q(0x6fe8)==0x246 && m.q(0x6ff0)==0x7007 && m.q(0x6ff8)==16);
    assert((m.q(0x3000)&0x60)==0x60 && (m.q(0x1000)&32));
    assert(emulateIret64(m,s)&&s.get(0x681e)==0x10000&&s.get(0x681c)==0x7007&&s.get(0x6820)==0x246);
    setup(m,s);m.bytes[0x5405]=0x8f;assert(emulateExternalEntry(m,s)&&s.get(0x6820)==0x246);
    m.q(s.get(0x681c)+16,0x4246);auto old=s.get(0x681c);assert(!emulateIret64(m,s)&&s.get(0x681c)==old);
    setup(m,s);m.bytes[0x5404]=1;auto before=m.bytes;
    assert(!emulateExternalEntry(m,s)&&m.bytes==before&&s.get(0x4016)==0x80000040);
    setup(m,s);s.set(0x681c,0x7017);m.writable[7]=false;before=m.bytes;
    assert(!emulateExternalEntry(m,s)&&m.bytes==before&&s.get(0x681e)==0x10000);m.writable[7]=true;
    setup(m,s);m.q(0x3000,0x81);before=m.bytes;assert(!emulateExternalEntry(m,s)&&m.bytes==before);
    s.set(0x6800,0x80000033);assert(emulateExternalEntry(m,s)); // supervisor WP0
    setup(m,s);m.q(0x3000,0x8000000000000083ull);assert(emulateExternalEntry(m,s)); // NX data permitted
    setup(m,s);m.q(0x3000,0x8000000000000083ull);s.set(0x2806,0x500);before=m.bytes;
    assert(!emulateExternalEntry(m,s)&&m.bytes==before); // NX reserved when NXE0
    setup(m,s);m.writable[1]=false;before=m.bytes;assert(!emulateExternalEntry(m,s)&&m.bytes==before);m.writable[1]=true;
    setup(m,s);m.q(0x3000,0x8003);
    for(unsigned i=0;i<512;++i)m.q(0x8000+i*8,uint64_t(i)*4096+3);
    assert(emulateExternalEntry(m,s)&&emulateIret64(m,s)); // four-level 4KiB paging
    setup(m,s);m.q(0x1000,0x2001);before=m.bytes;
    assert(!emulateExternalEntry(m,s)&&m.bytes==before); // parent RW restricts writable leaf
    setup(m,s);m.q(0x4008,0x00af1b000000ffffull);before=m.bytes;
    assert(!emulateExternalEntry(m,s)&&m.bytes==before); // non-present CS descriptor
    setup(m,s);m.readable[5]=false;before=m.bytes;
    assert(!emulateExternalEntry(m,s)&&m.bytes==before);m.readable[5]=true;
    setup(m,s);s.set(0x4824,1);assert(!emulateExternalEntry(m,s));
    s.set(0x4824,8);before=m.bytes;
    assert(validExternalEntry(s)&&!emulateExternalEntry(m,s)&&m.bytes==before); // native IRQ may nest, step helper must not lose NMI state
    s.set(0x4824,0);s.set(0x4016,0x80000202);assert(!emulateExternalEntry(m,s));
    s.set(0x4016,0x8000030d);assert(!emulateExternalEntry(m,s));
    // High-half IDT/GDT, handler and stack use the same low guest physical
    // backing. Verify frame/IRET addresses stay full-width throughout.
    constexpr uint64_t hi=0xffff800000000000ull;
    auto highSetup=[&](){
        setup(m,s);m.q(0x1800,0x2003);
        s.set(0x681e,hi+0x10000);s.set(0x681c,hi+0x7007);
        s.set(0x6816,hi+0x4000);s.set(0x6818,hi+0x5000);
        for(unsigned i=0;i<4;++i)m.bytes[0x5408+i]=uint8_t(hi>>(32+i*8));
    };
    highSetup();assert(emulateExternalEntry(m,s));
    assert(s.get(0x681e)==hi+0x11000&&s.get(0x681c)==hi+0x6fd8);
    assert(m.q(0x6fd8)==hi+0x10000&&m.q(0x6ff0)==hi+0x7007);
    assert(emulateIret64(m,s)&&s.get(0x681e)==hi+0x10000&&s.get(0x681c)==hi+0x7007);
    highSetup();s.set(0x681c,hi+7);before=m.bytes;
    assert(!emulateExternalEntry(m,s)&&m.bytes==before); // aligned frame crosses canonical hole
    highSetup();assert(emulateExternalEntry(m,s));m.q(0x6fd8,0x800000000000ull);
    old=s.get(0x681c);assert(!emulateIret64(m,s)&&s.get(0x681c)==old);
    puts("PASS: same-ring IRQ frame/IRETQ, alignment, flags, A/D, WP/NX, preflight and rejection without mutation");
}
