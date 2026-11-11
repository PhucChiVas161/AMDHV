#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
static unsigned failures;
static void check(bool ok,const char *s){printf("%s: %s\n",ok?"PASS":"FAIL",s);failures+=!ok;}
int main(){
    setvbuf(stdout,nullptr,_IONBF,0);
    if(hv_vm_create(HV_VM_DEFAULT))return 2;
    hv_vcpuid_t keeper;if(hv_vcpu_create(&keeper,HV_VCPU_DEFAULT)){hv_vm_destroy();return 2;}
    std::thread worker([&](){
    hv_vcpuid_t c;if(hv_vcpu_create(&c,static_cast<hv_vcpu_options_t>(2))){check(false,"create AP vCPU on second thread");return;}
    check(c==1,"AP uses slot1 and separate thread shared state");
    void *mem=nullptr;if(posix_memalign(&mem,4096,4096)){hv_vcpu_destroy(c);check(false,"allocate AP backing");return;}
    memset(mem,0,4096);auto b=static_cast<unsigned char*>(mem);
    const unsigned char code[]={0xfa,0x0f,0x09,0x66,0x89,0xc5,0x8c,0xc8,0x8e,0xd8,0x8e,0xc0,0x8e,0xd0,0x31,0xc0,0x8e,0xe0,0x8e,0xe8,
        0x0f,0x20,0xc0,0x66,0x83,0xc8,0,0x0f,0x22,0xc0,0xb8,0x34,0x12,0x0f,0xa2,0xf4};memcpy(b,code,sizeof(code));
    bool ok=!hv_vm_map(mem,0x86000,4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC);
    for(unsigned i=0;i<6;++i){ok &= !hv_vmx_vcpu_write_vmcs(c,0x800+i*2,0x8600);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x6806+i*2,0x86000);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x4800+i*2,0xffff);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x4814+i*2,i==1?0x9b:0x93);}
    hv_vcpu_write_register(c,HV_X86_RIP,0);hv_vcpu_write_register(c,HV_X86_RSP,0x800);
    hv_vmx_vcpu_write_vmcs(c,0x6800,0x30);hv_vcpu_write_register(c,HV_X86_RFLAGS,2);
    check(ok,"configure real AP-like CS selector8600/base86000, IP0");
    auto run=[&](){for(unsigned i=0;i<256;++i){auto r=hv_vcpu_run(c);if(r){printf("run=%x\n",r);return false;}
        uint64_t reason=0;hv_vmx_vcpu_read_vmcs(c,0x4402,&reason);if(reason!=1)return reason==10;}return false;};
    uint64_t rip=0,ax=0;bool ran=run();hv_vcpu_read_register(c,HV_X86_RIP,&rip);hv_vcpu_read_register(c,HV_X86_RAX,&ax);
    printf("real AP-like exit IP=%llx AX=%llx\n",rip,ax);
    check(ran&&rip==sizeof(code)-3&&ax==0x1234,"nonzero real-mode CS base fetches the AP vector rather than linear IP0");
    hv_vcpu_destroy(c);hv_vm_unmap(0x86000,4096);free(mem);
    });worker.join();hv_vcpu_destroy(keeper);hv_vm_destroy();return failures?1:0;
}
