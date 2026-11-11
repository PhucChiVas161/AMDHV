#include "vcpu_state.hpp"
#include <cassert>
#include <cstdio>
using namespace amdhv;
int main() {
    alignas(8) uint8_t rw[abi::RwStride] = {};
    VcpuState s; s.reset(); s.publish(rw);
    assert(s.get(0x681e) == 0 && s.get(0x6820) == 2);
    assert(s.get(0x6800) == 0x10 && s.registers[47] == 0xffff0ff0);
    // Simulate framework writes: RIP via VMCS cache; RAX via register array.
    unsigned rip = VcpuState::index(0x681e);
    VcpuState::write(rw, 8 + 8 * rip, 0x1234);
    VcpuState::write(rw, 0x300 + (rip / 64) * 8, 1ull << (rip % 64));
    VcpuState::write(rw, 0xc80 + 2 * 8, 0xaabbccdd);
    // Guest cannot forge read-only exit state by writing dirty bits.
    unsigned reason = VcpuState::index(0x4402);
    VcpuState::write(rw, 8 + 8 * reason, 123);
    VcpuState::write(rw, 0x308, VcpuState::read(rw, 0x308) | (1ull << (reason % 64)));
    s.pull(rw);
    assert(s.get(0x681e) == 0x1234 && s.registers[0] == 0x1234);
    assert(s.registers[2] == 0xaabbccdd && s.get(0x4402) == 0);
    // CR2 is a register-array slot, not a VMCS alias. Keep a full guest value
    // through framework pull/publish and independent vCPU snapshots.
    VcpuState other; other.reset();
    VcpuState::write(rw,0xc80+38*8,0xfedcba9876543210ull);
    s.pull(rw);
    assert(s.registers[38]==0xfedcba9876543210ull && other.registers[38]==0);
    s.registers[38]=0x123456789abcdef0ull; s.publish(rw);
    assert(VcpuState::read(rw,0xc80+38*8)==0x123456789abcdef0ull);
    s.set(0x4402, 10); s.publish(rw);
    assert(VcpuState::read(rw, 8 + 8 * reason) == 10);
    assert(!VcpuState::read(rw, 0x300) && !VcpuState::read(rw, 0x308));
    for (unsigned i = 1; i < 92; ++i)
        assert(VcpuState::read(rw, 0x2f0 + (i / 64) * 8) & (1ull << (i % 64)));
    assert(!VcpuState::index(0xffff));
    s.set(0x6c16, ~0ull); // unsupported host fields never index past storage
    assert(s.get(0x6c16) == 0);
    s.reset(); s.publish(rw);
    VcpuState::write(rw, 0, 0x108); VcpuState::write(rw, 0xc78, (uint64_t)-1234);
    s.pull(rw); assert(!s.tscSupported); // no silent interpretation for default-clock mode
    s.relativeTsc = true; s.pull(rw);
    assert(s.tscSupported && s.tscOffset == (uint64_t)-1234);
    s.publish(rw); VcpuState::write(rw, 0xc78, 888); s.pull(rw);
    assert(s.tscOffset == (uint64_t)-1234); // dirty bit required
    static_assert(managedMsrId(0xc0000083) == 3);
    static_assert(managedMsrId(0xc0000102) == 5);
    static_assert(managedMsrId(0xc0010117) == 0); // host HSAVE never exposed
    static_assert(!validManagedMsr(3, 0x800000000000ull));
    static_assert(validManagedMsr(3, 0xffff800000000000ull));
    static_assert(!validManagedMsr(4, 1ull << 32));
    s.reset(); s.publish(rw);
    VcpuState::write(rw, 0xc50, 1ull << 3);
    VcpuState::write(rw, 0x7d0 + 3*8, 0x12345678);
    s.pull(rw); assert(s.msrs[3] == 0x12345678);
    s.publish(rw); assert(!(VcpuState::read(rw, 0xc50) & (1ull << 3)));
    s.pull(rw); assert(s.msrs[3] == 0x12345678);
    static_assert(compactMsrId(148) == 6 && compactMsrId(149) == 7);
    static_assert(compactMsrId(0x100000003ull) == 0);
    static_assert(managedMsrId(0xc0000100) == 6 && managedMsrOffset(6) == 0x448);
    s.setMsrValue(6, 0x123000); assert(s.get(0x680e) == 0x123000);
    s.set(0x6810, 0x456000); assert(s.msrValue(7) == 0x456000);
    s.publish(rw); s.pull(rw); assert(s.msrValue(6) == 0x123000 && s.msrValue(7) == 0x456000);
    static_assert(compactMsrId(145) == 8 && compactMsrId(147) == 10);
    static_assert(managedMsrId(0x175) == 9 && managedMsrOffset(10) == 0x638);
    static_assert(!validManagedMsr(8, 1ull << 32));
    static_assert(!validManagedMsr(9, 0x800000000000ull));
    for (unsigned id = 8; id <= 10; ++id) {
        s.setMsrValue(id, 0x1000 + id);
        assert(s.get(managedMsrVmcs(id)) == 0x1000 + id);
        s.publish(rw); s.pull(rw);
        assert(s.msrValue(id) == 0x1000 + id);
    }
    s.setMsrValue(999, 1); assert(s.msrValue(999) == 0);
    s.reset(); s.relativeTsc = true; s.tscSnapshot = 1000; s.tscOffset = uint64_t(-50);
    s.publish(rw);
    assert(VcpuState::read(rw, 0xc68) + VcpuState::read(rw, 0xc70) == 950);
    VcpuState::write(rw, 0xc78, uint64_t(-75)); VcpuState::write(rw, 0, 0x108);
    s.pull(rw); assert(s.tscOffset == uint64_t(-75));
    s.tscSnapshot = 2000; s.publish(rw);
    assert(VcpuState::read(rw, 0xc68) + VcpuState::read(rw, 0xc70) == 1925);
    s.reset(); s.publish(rw); VcpuState::write(rw, 0, 0x88);
    s.pull(rw); assert(s.invalidFp); s.publish(rw); s.pull(rw); assert(s.invalidFp);
    s.reset(); assert(!s.invalidFp);
    alignas(16) uint8_t inputFp[512]={}, outputFp[512]={};
    s.reset(); s.publish(rw,outputFp);
    assert(outputFp[0]==0x7f && outputFp[1]==3 && outputFp[24]==0x80 && outputFp[25]==0x1f);
    for(unsigned i=0;i<512;++i) inputFp[i]=outputFp[i];
    inputFp[160]=0x5a; VcpuState::write(rw,0,0x88); s.pull(rw,inputFp);
    assert(!s.invalidFp && s.fp[160]==0x5a);
    inputFp[160]=0xa5; assert(s.fp[160]==0x5a); // private snapshot
    s.publish(rw,outputFp); assert(outputFp[160]==0x5a);
    s.pull(rw,inputFp); assert(s.fp[160]==0x5a); // dirty bit required
    inputFp[27]=0x80; VcpuState::write(rw,0,0x88); s.pull(rw,inputFp);
    assert(s.invalidFp); s.publish(rw,outputFp); s.pull(rw,inputFp); assert(s.invalidFp);
    inputFp[27]=0; VcpuState::write(rw,0,0x88); s.pull(rw,inputFp);
    assert(!s.invalidFp && s.fp[160]==0xa5); // explicit valid rewrite recovers
    s.publish(rw,outputFp,inputFp);
    assert(inputFp[160]==0xa5 && inputFp[24]==0x80);
    inputFp[0]=0x7e; VcpuState::write(rw,0,0x88); s.pull(rw,inputFp);
    assert(s.fp[0]==0x7e && s.fp[160]==0xa5); // partial update retains published payload
    s.reset(); assert(!s.invalidFp && s.fp[160]==0);
    for(unsigned i=64;i<sizeof(s.xsaveTail);++i)s.xsaveTail[i]=0xa5;
    s.xsaveTail[0]=4;VcpuState::write(rw,0,0x88);s.pull(rw,inputFp);
    assert(s.xsaveTail[0]==7);
    for(unsigned i=64;i<sizeof(s.xsaveTail);++i)assert(s.xsaveTail[i]==0xa5);
    s.publish(rw,outputFp,inputFp);
    for(unsigned i=64;i<sizeof(s.xsaveTail);++i)assert(s.xsaveTail[i]==0xa5);
    VcpuState extendedOther;extendedOther.reset();assert(extendedOther.xsaveTail[0]==3);
    for(unsigned i=1;i<sizeof(s.xsaveTail);++i)assert(!extendedOther.xsaveTail[i]);
    s.reset();assert(s.xsaveTail[0]==3);
    for(unsigned i=1;i<sizeof(s.xsaveTail);++i)assert(!s.xsaveTail[i]);
    puts("PASS: vCPU register aliases, VMCS dirty/valid masks, read-only exit isolation and bounds");
}
