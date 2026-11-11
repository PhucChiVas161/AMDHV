#include "vcpu_state.hpp"
#include "xstate_policy.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
extern "C" void fpu_roundtrip_test(void *, void *, void *, void *);
int main() {
    assert(__builtin_cpu_supports("avx"));
    alignas(64) unsigned char guest[4096]={}, scratch[8192]={}, before[832]={}, after[832]={};
    amdhv::VcpuState s; s.reset();
    memcpy(guest,s.fp,512);
    memcpy(guest+512,s.xsaveTail,sizeof(s.xsaveTail));
    *reinterpret_cast<uint64_t*>(scratch+amdhv::SwitchMaskOffset)=7;
    for (unsigned n=0;n<1000;++n) {
        fpu_roundtrip_test(guest,scratch,before,after);
        // Defined FXSAVE header, x87 register values, all XMM and all YMM upper halves.
        assert(memcmp(before,after,5)==0);
        assert(memcmp(before+6,after+6,26)==0);
        for (unsigned i=0;i<8;++i) assert(memcmp(before+32+16*i,after+32+16*i,10)==0);
        assert(memcmp(before+160,after+160,256)==0);
        assert(memcmp(before+512,after+512,8)==0);
        assert(memcmp(before+576,after+576,256)==0);
        for(unsigned i=0;i<16;++i) assert(guest[160+i]==0 && guest[400+i]==255);
#ifdef AMDHV_TEST_EXTENDED_FP
        for(unsigned i=0;i<16;++i)assert(guest[576+15*16+i]==255);
        assert((guest[512]&7)==7);
#endif
        // ST0 = 2.0 in x87 extended precision after the guest calculation.
        assert(guest[39]==0x80 && guest[40]==0 && guest[41]==0x40);
    }
#ifdef AMDHV_TEST_EXTENDED_FP
    puts("PASS: actual XSAVE switch macros retain guest YMM mutations and preserve host x87/XMM/YMM/init flags across 1000 runs");
#else
    puts("PASS: actual FP switch macros preserve host x87/XMM/YMM and init-state flags across 1000 guest mutations");
#endif
}
