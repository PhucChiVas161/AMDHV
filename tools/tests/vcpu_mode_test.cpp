#include "vcpu_mode.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>
using namespace amdhv;
static void mode(VcpuState &s, bool wide) {
    s.reset(); s.set(0x6800, wide ? 0x80000011 : 0x11);
    s.set(0x6804, wide ? 0x20 : 0); s.set(0x2806, wide ? 0x500 : 0);
    s.set(0x6802, wide ? 0x1000 : 0);
    for (unsigned i = 0; i < 6; ++i) {
        s.set(0x800 + i*2, i == 1 ? 8 : 16);
        s.set(0x4800 + i*2, 0xffffffff);
        s.set(0x4814 + i*2, i == 1 ? (wide ? 0xa09b : 0xc09b) : 0xc093);
    }
}
int main() {
    VcpuState nmi;mode(nmi,true);nmi.set(0x4016,NmiEntry);
    assert(validNativeNmiEntry(nmi,true)&&validPilotControls(nmi,true));
    assert(!validNativeNmiEntry(nmi,false)&&!validPilotControls(nmi));
    for(uint64_t shadow : {1ull,2ull,8ull,9ull,10ull}) {
        nmi.set(0x4824,shadow);nmi.set(0x6820,0x202);
        assert(validPilotMode(nmi,true)&&!validNativeNmiEntry(nmi,true)&&!validPilotControls(nmi,true));
    }
    nmi.set(0x4016,0);nmi.set(0x4824,8);assert(needsVirtualNmi(nmi));
    assert(validPilotMode(nmi,true)&&!validPilotMode(nmi));
    nmi.set(0x4016,0x80000040);assert(validExternalEntry(nmi)&&validPilotControls(nmi,true));
    for(uint64_t shadow : {3ull,4ull,11ull,16ull}) {nmi.set(0x4824,shadow);assert(!validPilotMode(nmi,true));}
    nmi.set(0x4824,1);nmi.set(0x6820,2);assert(!validPilotMode(nmi,true));
    nmi.set(0x4824,0);nmi.set(0x4016,0x80000203);assert(!validPilotControls(nmi,true));
    mode(nmi,false);nmi.set(0x4824,8);assert(!validPilotMode(nmi,true));
    for(bool code64 : {false,true}) {
        VcpuState id;mode(id,code64);id.set(0x6820,0x200202);
        assert(validPilotMode(id)&&validPilotMode(id,true));
        for(uint64_t forbidden : {0x100ull,0x1000ull,0x2000ull,0x4000ull,0x20000ull,0x80000ull,0x100000ull,1ull<<32}) {
            id.set(0x6820,0x200202|forbidden);assert(!validPilotMode(id,true));
        }
    }
    VcpuState fault;mode(fault,true);
    static_assert(supportedNativeCr4(0,0)==0);
    static_assert(supportedNativeCr4(0x100081,4)==NativeCr4Mask);
    for(uint64_t bit : {0x800ull,0x10000ull,0x100000ull,0x200000ull,0x400000ull}) {
        mode(fault,true);fault.set(0x6804,0x20|bit);
        assert(validPilotMode(fault,true)&&!validPilotMode(fault));
        fault.set(0x4816,0xc09b);assert(!validPilotMode(fault,true));
        mode(fault,false);fault.set(0x6804,bit);assert(!validPilotMode(fault,true));
    }
    mode(fault,true);fault.set(0x6820,0x40002);
    assert(validPilotMode(fault,true)&&!validPilotMode(fault));
    fault.set(0x6804,0x40020);assert(validPilotMode(fault,true)&&!validPilotMode(fault));
    for(uint64_t xcr : {1ull,3ull,7ull,0xe7ull,0x201ull,0x207ull,0x2e7ull}) {
        fault.registers[50]=xcr;assert(validGuestXcr0(xcr)&&validPilotMode(fault,true));
    }
    for(uint64_t xcr : {0ull,2ull,4ull,5ull,8ull,0x27ull,0x87ull,0xe3ull,0x1001ull,1ull<<32}) {
        fault.registers[50]=xcr;assert(!validGuestXcr0(xcr)&&!validPilotMode(fault,true));
    }
    mode(fault,true);
    assert(!(faultIntercepts(fault)&(1u<<3)));
    fault.set(0x4004,8);assert(faultIntercepts(fault)&8);fault.set(0x4004,0);
    fault.set(0x4016,0x80000603);assert(!validFaultEntry(fault));fault.set(0x4016,0);
    assert(nativeFaultMode(fault,true)&&!nativeFaultMode(fault,false));
    assert(!(faultIntercepts(fault)&(1u<<14))&&(faultIntercepts(fault)&(1u<<1))&&(faultIntercepts(fault)&(1u<<18)));
    fault.set(0x4004,1u<<14);assert(faultIntercepts(fault)&(1u<<14));
    fault.set(0x4008,0xffffffff);assert(!(faultIntercepts(fault)&(1u<<14)));
    fault.set(0x4004,0);assert(faultIntercepts(fault)&(1u<<14));
    fault.set(0x4008,0);fault.set(0x4006,1);assert(!validPilotControls(fault,true));fault.set(0x4006,0);
    for(unsigned v : {0u,6u,7u,8u,10u,11u,12u,13u,14u,16u,17u,19u}){
        fault.set(0x4016,faultExitInfo(v));fault.set(0x4018,0);
        assert(validFaultEntry(fault)&&validPilotControls(fault,true)&&!validPilotControls(fault));
        fault.set(0x4016,faultExitInfo(v)^0x800);assert(!validFaultEntry(fault));
    }
    fault.set(0x4016,0x80000302);assert(!validFaultEntry(fault)); // NMI not a supported synchronous fault
    fault.set(0x4016,0x80000b0e);fault.set(0x4018,5);
    assert(svmEntryEvent(fault)==0x0000000580000b0eull);
    fault.set(0x4018,1ull<<32);assert(!validFaultEntry(fault));
    fault.set(0x4016,0);fault.set(0x4018,0);fault.set(0x6820,0x10002);
    assert(validPilotMode(fault,true)&&!validPilotMode(fault)); // RF is guest state only in fast mode
    VcpuState cr2State; mode(cr2State,true);
    cr2State.registers[38]=0xfedcba9876543210ull;
    assert(validPilotMode(cr2State)); // CR2 payload is not a host address.
    cr2State.set(0x4820,0x10000);assert(validPilotMode(cr2State));
    cr2State.set(0x80e,0x18);cr2State.set(0x6814,0x9000);
    cr2State.set(0x480e,0x67);assert(validPilotMode(cr2State));
    cr2State.set(0x80c,0x28);cr2State.set(0x6812,0xb000);
    cr2State.set(0x480c,7);cr2State.set(0x4820,0x82);assert(validPilotMode(cr2State));
    cr2State.set(0x4822,0x89);assert(!validPilotMode(cr2State)); // available TSS not a loaded TR
    cr2State.set(0x4822,0x8b);cr2State.set(0x80e,0x1c);assert(!validPilotMode(cr2State)); // TI
    cr2State.set(0x80e,0x18);cr2State.set(0x6814,0x800000000000ull);assert(!validPilotMode(cr2State));
    cr2State.set(0x6814,0xffff800000009000ull);assert(validPilotMode(cr2State));
    cr2State.set(0x480e,0x66);assert(!validPilotMode(cr2State));
    cr2State.set(0x480e,0x67);cr2State.set(0x4822,0xa08b);assert(!validPilotMode(cr2State));
    cr2State.set(0x4822,0x8b);cr2State.set(0x4820,0x10000);assert(!validPilotMode(cr2State)); // non-null unusable LDT
    mode(cr2State,true);
    for(unsigned i : {0u,2u,3u,4u,5u}){
        cr2State.set(0x800+i*2,0);cr2State.set(0x4814+i*2,0x10000);
        assert(validPilotMode(cr2State));
    }
    cr2State.set(0x4818,0x8093);assert(validPilotMode(cr2State)); // null SS cache with CPU-derived G
    cr2State.set(0x4818,0xf3);assert(!validPilotMode(cr2State)); // no ring3 cache widening
    cr2State.set(0x4818,0x10000);cr2State.set(0x680e,0xffff800000001000ull);assert(validPilotMode(cr2State));
    cr2State.set(0x680e,0x800000000000ull);assert(!validPilotMode(cr2State));
    // Reproduce actual Zen5 save images, then validate the shared output
    // serializer that the kernel and explicit diagnostic probe both use.
    mode(cr2State,true);cr2State.set(0x80e,0x18);cr2State.set(0x6814,0x9000);
    cr2State.set(0x480e,0x67);cr2State.set(0x4822,0x89);assert(!validPilotMode(cr2State));
    cr2State.set(0x4822,vmxSavedCacheAttributes(7,0x89,0x67));assert(validPilotMode(cr2State));
    cr2State.set(0x804,0);cr2State.set(0x4804,0);cr2State.set(0x4818,0x4000);assert(!validPilotMode(cr2State));
    cr2State.set(0x4818,vmxSavedCacheAttributes(2,0x400,0));assert(validPilotMode(cr2State));
    static_assert(svmSegmentAttributes(0xa09b) == 0xa9b);
    static_assert(svmSegmentAttributes(0xc093) == 0xc93);
    for (unsigned ar = 0; ar < 4096; ++ar)
        assert(svmSegmentAttributes(vmxSegmentAttributes(ar)) == ar);
    static_assert(vmxSavedSegmentAttributes(0x49b, 0xffffffff) == 0xc09b);
    static_assert(vmxSavedSegmentAttributes(0x29b, 0xffffffff) == 0xa09b);
    static_assert(vmxSavedCacheAttributes(7,0x89,0x67)==0x8b);
    static_assert(vmxSavedCacheAttributes(7,0x8b,0x67)==0x8b);
    static_assert(vmxSavedCacheAttributes(7,0x81,0x67)==0x81); // no unsupported TSS16 widening
    static_assert(vmxSavedCacheAttributes(2,0x400,0)==0x10000); // AMD null SS retains DB
    static_assert(vmxSavedCacheAttributes(6,0,0)==0x10000);
    static_assert(vmxSavedCacheAttributes(0,0x92,0xffffffff)==0x8093);
    static_assert(vmxSavedCacheAttributes(1,0x29b,0xffffffff)==0xa09b);
    static_assert(vmxSavedCacheAttributes(2,0x493,0xffffffff)==0xc093);
    static_assert(vmxSavedSegmentAttributes(0x9b, 0xffff) == 0x9b);
    static_assert(vmxSavedSegmentAttributes(0x89b, 0xfffff) == 0x9b);
    VcpuState s; s.reset(); assert(validPilotMode(s));
    s.set(0x6800, 0x11); assert(validPilotMode(s)); // cached real segments after PE
    s.set(0x802, 3); assert(!validPilotMode(s)); // ring 3 rejected
    s.reset(); s.set(0x6816,0x12340000); s.set(0x4810,31);
    s.set(0x6818,0x56780000); s.set(0x4812,2047); assert(validPilotMode(s));
    s.set(0x4810,0x10000); assert(!validPilotMode(s));
    s.set(0x4810,31); s.set(0x6818,0x100000000ull); assert(!validPilotMode(s));
    mode(s, false); assert(validPilotMode(s));
    s.set(0x681e, 0x100000000ull); assert(!validPilotMode(s));
    mode(s, true); assert(validPilotMode(s));
    s.set(0x4816, vmxSavedSegmentAttributes(0x29b, 0xffffffff));
    assert(validPilotMode(s)); // resume after hardware drops CS.G

    s.set(0x2806, 0x100); assert(!validPilotMode(s)); // missing LMA
    mode(s, true); s.set(0x6804, 0); assert(!validPilotMode(s));
    mode(s, true); s.set(0x6802, 0x1001); assert(!validPilotMode(s));
    mode(s, true); s.set(0x681e, 0x800000000000ull); assert(!validPilotMode(s));
    mode(s, true); s.set(0x6820, 0x202); assert(validPilotMode(s));
    mode(s, true); s.set(0x4816, 0xc09b); assert(validPilotMode(s)); // IA-32e compatibility code
    mode(s, true); s.set(0x802, 11); assert(!validPilotMode(s));
    mode(s, true); s.setMsrValue(6, 0xffff800000001000ull); assert(validPilotMode(s));
    s.setMsrValue(7, 0x800000000000ull); assert(!validPilotMode(s));
    mode(s, true); s.set(0x6816,0x800000000000ull); assert(!validPilotMode(s));
    mode(s, false); s.set(0x4816,0x409b); s.set(0x4802,0xffff); assert(validPilotMode(s));
    s.set(0x4816,0x40fb); assert(!validPilotMode(s)); // ring3 code not supported
    mode(s, false); s.set(0x6800,0x23); s.set(0x6804,0x2660);
    s.set(0x6002,0x2000); s.set(0x2806,0x100); s.set(0x6802,0x800000);
    s.set(0x4002,0x200080); assert(validPilotMode(s) && validPilotControls(s));
    s.set(0x4012,0x200); assert(!validPilotControls(s)); // LME is not yet LMA
    s.set(0x6800,0x80000023); assert(!validPilotMode(s)); // PG with LMA missing
    s.set(0x2806,0x500); assert(validPilotMode(s) && validPilotControls(s));
    s.set(0x802,0x18); s.set(0x4816,0xa09b); assert(validPilotMode(s));
    s.set(0x4816,0xe09b); assert(!validPilotMode(s)); // L and DB cannot both be set
    s.set(0x4816,0xa09b); s.set(0x6804,0x2640); assert(!validPilotMode(s));
    s.set(0x6804,0x2660); s.set(0x4012,0x400); assert(!validPilotControls(s));
    mode(s,true); s.set(0x6800,0x80050011); s.set(0x6804,0xb0);
    assert(validPilotMode(s));
    s.set(0x6804,0x90); assert(!validPilotMode(s)); // PAE cannot disappear in IA32e
    s.set(0x6804,0x400b0); assert(!validPilotMode(s)); // no OSXSAVE
    s.set(0x6804,0xb0); s.set(0x6800,0xc0050011); assert(!validPilotMode(s)); // no CD
    for(uint64_t efer : {0ull,0x100ull,0x500ull}) {
        if(efer==0x500) mode(s,true); else mode(s,false);
        if(efer==0x100) s.set(0x6804,0x20);
        s.set(0x2806,efer|0x800); assert(validPilotMode(s));
        if(efer==0x500) s.set(0x4012,0x200);
        assert(validPilotControls(s));
        s.set(0x2806,efer|0x801); assert(!validPilotMode(s)); // SCE must not clear internal TF in step mode
        assert(validPilotMode(s,true)==(efer!=0)); // fast accepts SCE while LME is prepared, before LMA
        if(efer==0x100) {
            s.set(0x6804,0);assert(!validPilotMode(s,true)); // prepared LME still requires PAE
            s.set(0x6804,0x20);s.set(0x6800,0x10);assert(!validPilotMode(s,true)); // and PE
            s.set(0x6800,0x11);s.set(0x4816,0xc0fb);assert(!validPilotMode(s,true)); // ring0 only
            s.set(0x4816,0xc09b);
        }
        s.set(0x2806,efer|0x1800); assert(!validPilotMode(s)); // guest SVME forbidden
    }
    uint64_t target=0;
    assert(flagsStepTarget(0xff00,0xff01,0xffff0000,false,0xffff,target) && target==0xffffff01);
    assert(flagsStepTarget(0x8000,0x8002,0,true,0xffffffff,target) && target==0x8002);
    assert(!flagsStepTarget(0xffff,0x10000,0,false,0xffff,target));
    assert(!flagsStepTarget(0x10,0x11,0xfffffff0,false,0xffff,target));
    assert(!flagsStepTarget(0x8000,0x8000,0,true,0xffffffff,target));
    assert(!flagsStepTarget(0x8000,0x8010,0,true,0xffffffff,target));
    assert(!flagsStepTarget(0x7fffffffffff,0x800000000000,0,true,~0ull,target));
    assert(flagsStepTarget(0xffffffff810441b3ull,0xffffffff810441b5ull,0,true,~0ull,target)&&target==0xffffffff810441b5ull);
    assert(!flagsStepTarget(0xffff7fffffffffffull,0xffff800000000000ull,0,true,~0ull,target));
    assert(!flagsStepTarget(~0ull,0,0,true,~0ull,target));
    mode(s,true);s.set(0x681e,0xffffffff810441b3ull);s.set(0x681c,0xffff800000009087ull);assert(validPilotMode(s));
    s.set(0x681c,0xffff7fffffffffff);assert(!validPilotMode(s));
    mode(s,true);s.set(0x4818,0x8093);assert(validPilotMode(s)); // code64 data cache DB ignored
    s.set(0x4818,0x80f3);assert(!validPilotMode(s)); // still no CPL3 data cache
    assert(flagsStepComplete(0x41,0x8001,0x8001,1));
    assert(!flagsStepComplete(0x41,0x8001,0x8001,0x4000));
    assert(!flagsStepComplete(0x41,0x8002,0x8001,1));
    assert(!flagsStepComplete(0x400,0x8001,0x8001,1));
    assert(flagsStepBoundary(0x72,0x8001,0x8001,0));
    assert(flagsStepBoundary(0x77,0x8001,0x8001,0));
    assert(!flagsStepBoundary(0x77,0x8000,0x8001,0));
    assert(flagsStepBoundary(0x400,0x8001,0x8001,0));
    assert(!flagsStepBoundary(0x72,0x8000,0x8001,0));
    assert(!flagsStepBoundary(0x41,0x8001,0x8001,0));
    assert(!flagsStepBoundary(0xffff,0x8001,0x8001,1));
    mode(s,true);s.set(0x6820,0x402);assert(validPilotMode(s)); // DF supported
    s.set(0x6820,0x602);assert(validPilotMode(s));
    for (unsigned shadow : {0u,1u,2u}) {s.set(0x4824,shadow);assert(validPilotMode(s));}
    for (unsigned shadow : {3u,4u,8u,9u,10u}) {s.set(0x4824,shadow);assert(!validPilotMode(s));}
    s.set(0x4824,1);s.set(0x6820,2);assert(!validPilotMode(s));
    s.set(0x4824,2);assert(validPilotMode(s));
    s.set(0x4002,4);assert(validPilotControls(s));
    s.set(0x4016,0x80000040);assert(!validPilotControls(s));
    s.set(0x4824,0);s.set(0x6820,0x202);assert(validPilotControls(s));
    s.set(0x4016,0x80000202);assert(!validPilotControls(s));
    s.set(0x4016,0);s.set(0x4002,0x400000);assert(!validPilotControls(s));
    puts("PASS: mode validation and VMX/SVM segment conversion");
}
