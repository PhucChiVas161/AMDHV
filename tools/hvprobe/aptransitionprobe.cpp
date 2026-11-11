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
    hv_vcpuid_t c;if(hv_vcpu_create(&c,static_cast<hv_vcpu_options_t>(2))){check(false,"create AP");return;}
    check(c==1,"EFI transition uses independent AP slot1/thread");
    void *mem=nullptr;if(posix_memalign(&mem,4096,4*4096)){hv_vcpu_destroy(c);check(false,"allocate AP backing");return;}
    memset(mem,0,4*4096);auto b=static_cast<unsigned char*>(mem);
    const unsigned char code[]={0x66,0x89,0xc5,0x8c,0xc8,0x8e,0xd8,0x8e,0xc0,0x8e,0xd0,0x31,0xc0,0x8e,0xe0,0x8e,0xe8,0xbe,0xd,0x11,0x66,0x8b,0x1c,0xbe,0x2d,0x11,0x66,0x8b,0x14,0x66,0xbf,0x6d,0x11,0x0,0x0,0xbe,0xf9,0x10,0x2e,0x66,0xf,0x1,0x14,0xbe,0x3,0x11,0x2e,0x66,0xf,0x1,0x1c,0xf,0x20,0xc0,0x66,0x83,0xc8,0x3,0xf,0x22,0xc0,0x2e,0x66,0xff,0x2d};memcpy(b,code,sizeof(code));
    const uint64_t gdt[]={0,0x00cf9b000000ffffull,0x00cf93000000ffffull};memcpy(b+0x1800,gdt,sizeof(gdt));
    auto w16=[&](unsigned o,uint16_t v){memcpy(b+o,&v,2);};auto w32=[&](unsigned o,uint32_t v){memcpy(b+o,&v,4);};
    w16(0x10f9,sizeof(gdt)-1);w32(0x10fb,0x87800);w16(0x1103,0);w32(0x1105,0);
    w32(0x110d,0x86000);w32(0x112d,0x10);w32(0x116d,0x88000);w16(0x1171,8);
    const unsigned char target[]={0x8e,0xda,0x8e,0xc2,0x8e,0xe2,0x8e,0xea,0x8e,0xd2,0xb8,0x78,0x56,0x34,0x12,0x0f,0xa2};
    memcpy(b+0x2000,target,sizeof(target));
    bool ok=!hv_vm_map(mem,0x86000,4*4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC);
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
    check(ran&&rip==0x8800f&&ax==0x12345678,"actual EFI AP prefix sets nonzero segments/GDTR/IDTR, enters PE and far-jumps to flat32");
    hv_vcpu_destroy(c);hv_vm_unmap(0x86000,4*4096);free(mem);});worker.join();hv_vcpu_destroy(keeper);hv_vm_destroy();return failures?1:0;
}
