#include "vcpu_mode.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>
using namespace amdhv;
static VcpuState user(){
    VcpuState s;s.reset();s.set(0x6800,0x80010033);s.set(0x6802,0x1000);s.set(0x6804,0x20);s.set(0x2806,0xd01);
    for(unsigned i=0;i<6;++i){s.set(0x4800+i*2,0xffffffff);s.set(0x800+i*2,i==1?0x33:0);
        s.set(0x4814+i*2,i==1?0xa0fb:0x10000);}
    s.set(0x804,0x2b);s.set(0x4818,0xc0f3);s.set(0x80e,0x38);s.set(0x6814,0xffff80000000e000ull);
    s.set(0x480e,0xfff);s.set(0x4822,0x8b);s.set(0x681e,0x10000a000);s.set(0x681c,0x10000b087);
    s.set(0x680e,0x10000b200);s.set(0x6810,0x10000b300);return s;
}
int main(){
    auto s=user();assert(validPilotMode(s,true)&&!validPilotMode(s)&&guestCpl(s)==3);
    // Exact segment/cache shape published by the default Alpine run on 0.3.32.
    auto linux=s;linux.set(0x80e,0x40);linux.set(0x6814,0xfffffe0000003000ull);
    linux.set(0x480e,0x4087);linux.set(0x6816,0xfffffe0000001000ull);linux.set(0x4810,0x7f);
    linux.set(0x6818,0xfffffe0000000000ull);linux.set(0x4812,0xfff);
    linux.set(0x681e,0x7fb6ebdb3125);linux.set(0x681c,0x7fffffffd000ull);
    linux.set(0x680e,0x7fb6ebf44b28);linux.set(0x6810,0);
    for(unsigned i:{0u,3u,4u,5u})linux.set(0x4800+i*2,0);
    assert(validPilotMode(linux,true));
    for(unsigned cpl:{1u,2u}){auto bad=s;bad.set(0x802,0x30|cpl);bad.set(0x4816,0xa09b|(cpl<<5));
        bad.set(0x804,0x28|cpl);bad.set(0x4818,0xc093|(cpl<<5));assert(!validPilotMode(bad,true));}
    for(auto pair:{std::initializer_list<uint64_t>{0x802,0x30},{0x802,0x37},{0x4816,0xa09b},
        {0x804,0x28},{0x4818,0xc093},{0x4818,0x10060},{0x80e,0},{0x6808,0x1000},
        {0x680e,0x800000000000ull},{0x4816,0xe0fb},{0x6820,0x3002}}){
        auto b=s;auto p=pair.begin();unsigned f=unsigned(*p++);b.set(f,*p);assert(!validPilotMode(b,true));}
    s.set(0x800,0x2b);s.set(0x4814,0xc0f3);assert(validPilotMode(s,true));
    s.set(0x800,0x10);s.set(0x4814,0xc093);assert(!validPilotMode(s,true));
    static_assert(vmxSavedCplAttributes(2,0xc93,0xffffffff,3)==0xc0f3);
    static_assert(vmxSavedCplAttributes(2,0x400,0,0)==0x10000);
    static_assert(vmxSavedCplAttributes(2,0x400,0,3)==0x10060);
    static_assert(vmxSavedCplAttributes(1,0x2fb,0xffffffff,3)==0xa0fb);
    for(uint64_t x:{0ull,2ull,0x14ull,0x20ull,0x3full,0x6full,0x78ull,0x79ull,0x7cull,0x89ull})assert(privilegedIntercept(x));
    for(uint64_t x:{0x4dull,0x60ull,0x64ull,0x72ull,0x77ull,0x7bull,0x8dull,0x400ull})assert(!privilegedIntercept(x));
    puts("PASS: fast CPL3/flat segments/TSS, strict privilege consistency, SS.DPL normalization and guarded intercept classes");
}
