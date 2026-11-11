#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main() {
    if(hv_vm_create(HV_VM_DEFAULT))return 2;
    hv_vcpuid_t c;if(hv_vcpu_create(&c,HV_VCPU_DEFAULT)){hv_vm_destroy();return 2;}
    void *p=nullptr;if(posix_memalign(&p,4096,4096))return 2;
    memset(p,0,4096);
    // 100,000 iterations: EAX++ / ECX-- / JNZ; bounded host exits must preserve progress.
    const unsigned char code[]={0xb9,0xa0,0x86,0x01,0,0x31,0xc0,0x40,0x49,0x75,0xfc,0xf4};
    memcpy(p,code,sizeof(code));
    bool ok=!hv_vm_map(p,0,4096,HV_MEMORY_READ|HV_MEMORY_EXEC);
    ok &= !hv_vmx_vcpu_write_vmcs(c,VMCS_GUEST_CR0,0x11);
    for(unsigned i=0;i<6;++i) {
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x800+i*2,i==1?8:16);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x4800+i*2,0xffffffff);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x4814+i*2,i==1?0xc09b:0xc093);
    }
    unsigned calls=0;bool halted=false;auto start=std::chrono::steady_clock::now();
    for(;ok && calls<1024;) {
        ++calls;auto r=hv_vcpu_run(c);uint64_t reason=~0ull;
        ok = !r && !hv_vmx_vcpu_read_vmcs(c,VMCS_RO_EXIT_REASON,&reason);
        if(ok && reason==12){halted=true;break;}
        ok &= reason==1;
    }
    double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    uint64_t ax=0,cx=0;
    ok &= halted && calls>200 && !hv_vcpu_read_register(c,HV_X86_RAX,&ax) && ax==100000 &&
        !hv_vcpu_read_register(c,HV_X86_RCX,&cx) && cx==0;
    printf("%s: 100000 loop iterations, %u bounded calls, %.6f seconds (not production performance)\n",ok?"PASS":"FAIL",calls,elapsed);
    ok &= !hv_vm_unmap(0,4096);free(p);ok &= !hv_vcpu_destroy(c) && !hv_vm_destroy();return ok?0:1;
}
