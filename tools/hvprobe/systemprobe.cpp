#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "segment_diagnostic.hpp"
#include <sys/sysctl.h>
static unsigned failures;
static bool diagnosticTrRepair;
static bool fastMode;
static void check(bool ok,const char *s){printf("%s: %s\n",ok?"PASS":"FAIL",s);failures+=!ok;}
static bool field(hv_vcpuid_t c,unsigned f,uint64_t wanted){uint64_t v=~0ull;auto r=hv_vmx_vcpu_read_vmcs(c,f,&v);
    if(r||v!=wanted)printf("VMCS%x=%llx expected=%llx r=%x\n",f,v,wanted,r);return !r&&v==wanted;}
static bool reg(hv_vcpuid_t c,hv_x86_reg_t r,uint64_t wanted){uint64_t v=~0ull;return !hv_vcpu_read_register(c,r,&v)&&v==wanted;}
static void mode(bool wide){
    printf("SYSTEM mode%u\n",wide?64:32);
    if(hv_vm_create(HV_VM_DEFAULT)){check(false,"create VM");return;}
    hv_vcpuid_t c;if(hv_vcpu_create(&c,HV_VCPU_DEFAULT)){check(false,"create vCPU");hv_vm_destroy();return;}
    void *mem=nullptr;if(posix_memalign(&mem,4096,8*4096)){check(false,"allocate");hv_vcpu_destroy(c);hv_vm_destroy();return;}
    memset(mem,0,8*4096);auto b=static_cast<unsigned char*>(mem);auto code=b+4*4096;auto t=static_cast<uint64_t*>(mem);
    t[0]=0x2003;t[512]=0x3003;t[1024]=0x4003;for(unsigned i=8;i<12;++i)t[1536+i]=uint64_t(i)*4096+3;
    bool ok=!hv_vm_map(mem,0x1000,4*4096,HV_MEMORY_READ|HV_MEMORY_WRITE)&&
        !hv_vm_map(code,0x8000,4*4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC);
    ok &= !hv_vmx_vcpu_write_vmcs(c,0x6800,wide?0x80000011:0x11);
    ok &= !hv_vmx_vcpu_write_vmcs(c,0x6804,wide?0x20:0);
    ok &= !hv_vmx_vcpu_write_vmcs(c,0x2806,wide?0x500:0);
    ok &= !hv_vmx_vcpu_write_vmcs(c,0x6802,wide?0x1000:0);
    for(unsigned i=0;i<6;++i){
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x800+i*2,i==1?8:16);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x4800+i*2,0xffffffff);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x4814+i*2,i==1?(wide?0xa09b:0xc09b):0xc093);
    }
    uint64_t gdt[]={0,wide?0x00af9b000000ffffull:0x00cf9b000000ffffull,0x00cf93000000ffffull,
                    0x0000890090000067ull,0,0x00008200b0000007ull,0};
    memcpy(code+0x2000,gdt,sizeof(gdt));
    ok &= !hv_vmx_vcpu_write_vmcs(c,0x6816,0xa000)&&!hv_vmx_vcpu_write_vmcs(c,0x4810,sizeof(gdt)-1);
    check(ok,"configure guest GDT, available TSS and LDT");
    // This probe verifies descriptor faults as exits. Native exception delivery
    // has its own real-IDT probe; intercept all faults here in fast mode.
    if(fastMode)hv_vmx_vcpu_write_vmcs(c,0x4004,0xffffffffu);
    auto run=[&](std::initializer_list<unsigned char> bytes) -> hv_return_t {
        unsigned i=0;for(auto v:bytes)code[i++]=v;
        hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vcpu_write_register(c,HV_X86_RFLAGS,2);
        for(unsigned n=0;n<128;++n){auto r=hv_vcpu_run(c);
            if(diagnosticTrRepair&&!normalizeDiagnosticSegments(c))return HV_ERROR;
            if(r)return r;uint64_t reason=0;
            hv_vmx_vcpu_read_vmcs(c,VMCS_RO_EXIT_REASON,&reason);if(reason!=1)return HV_SUCCESS;}
        return HV_ERROR;
    };
    check(run({0x31,0xc0,0x0f,0x00,0xd0,0x0f,0xa2})==0&&field(c,VMCS_RO_EXIT_REASON,10)&&field(c,0x80c,0)&&field(c,0x4820,0x10000),
          "LLDT null publishes an unusable guest LDTR");
    check(run({0x31,0xc0,0xb8,0x18,0,0,0,0x0f,0x00,0xd8,0x0f,0xa2})==0&&field(c,VMCS_RO_EXIT_REASON,10)&&
          field(c,0x80e,0x18)&&field(c,0x6814,0x9000)&&field(c,0x480e,0x67)&&field(c,0x4822,0x8b),
          "LTR loads guest TSS and publishes its hidden cache");
    uint64_t busy=0;memcpy(&busy,code+0x2018,8);
    check(((busy>>40)&15)==11,"hardware LTR writes guest GDT busy bit");
    check(run({0x31,0xc0,0x66,0x0f,0x00,0xc8,0x0f,0xa2})==0&&reg(c,HV_X86_RAX,0x18)&&field(c,0x6814,0x9000),
          "STR and TSS hidden state survive a separate run");
    check(run({0xb8,0x28,0,0,0,0x0f,0x00,0xd0,0x0f,0xa2})==0&&field(c,0x80c,0x28)&&field(c,0x6812,0xb000)&&field(c,0x4820,0x82),
          "LLDT loads guest LDT cache");
    check(run({0x31,0xc0,0x66,0x0f,0x00,0xc0,0x0f,0xa2})==0&&reg(c,HV_X86_RAX,0x28),"SLDT survives a separate run");
    check(run({0x31,0xc0,0x0f,0x00,0xd0,0x0f,0xa2})==0&&field(c,0x4820,0x10000),"LLDT null clears a previously loaded LDT");
    if(wide){
        check(run({0x31,0xc0,0x8e,0xd8,0x8e,0xc0,0x8e,0xd0,0x8e,0xe0,0x8e,0xe8,0x0f,0xa2})==0&&
              field(c,VMCS_RO_EXIT_REASON,10)&&field(c,0x800,0)&&field(c,0x804,0)&&field(c,0x806,0)&&field(c,0x808,0)&&field(c,0x80a,0),
              "Linux long entry clears all five data segment selectors");
        check(run({0x0f,0xa2})==0&&field(c,VMCS_RO_EXIT_REASON,10)&&field(c,0x80e,0x18),
              "null long-mode data segments and TSS survive re-entry");
    }
    auto busyResult=run({0xb8,0x18,0,0,0,0x0f,0x00,0xd8,0x0f,0xa2});
    check((fastMode&&wide?busyResult==0&&field(c,VMCS_RO_EXIT_REASON,0)&&field(c,0x4404,0x80000b0d):busyResult==HV_UNSUPPORTED)&&
          reg(c,HV_X86_RIP,0x8005)&&field(c,0x80e,0x18),
          "busy TSS reload faults without advancing guest RIP or replacing TR");
    // A fresh available TSS descriptor on a read-only GDT must cause an NPF,
    // rather than changing guest TR or bypassing the guest busy-bit write.
    gdt[3]=0x0000890090000067ull;memcpy(code+0x2000,gdt,sizeof(gdt));
    check(!hv_vm_protect(0xa000,4096,HV_MEMORY_READ),"protect guest GDT read-only");
    check(run({0xb8,0x18,0,0,0,0x0f,0x00,0xd8,0x0f,0xa2})==0&&field(c,VMCS_RO_EXIT_REASON,48),
          "LTR busy-bit store honors NPT write protection");
    memcpy(&busy,code+0x2018,8);check(((busy>>40)&15)==9,"faulting LTR leaves GDT descriptor available");
    hv_vmx_vcpu_write_vmcs(c,0x6816,0xc000);
    if(wide){
        auto pageResult=run({0xb8,0x28,0,0,0,0x0f,0x00,0xd0,0x0f,0xa2});
        check((fastMode?pageResult==0&&field(c,VMCS_RO_EXIT_REASON,0)&&field(c,0x4404,0x80000b0e):pageResult==HV_UNSUPPORTED)&&reg(c,HV_X86_RIP,0x8005),
              "nonpresent guest GDT PTE stays a guest page fault, without consuming LLDT");
        t[1536+12]=0xc003; // guest-present GPA, intentionally absent from NPT
    }
    check(run({0xb8,0x28,0,0,0,0x0f,0x00,0xd0,0x0f,0xa2})==0&&field(c,VMCS_RO_EXIT_REASON,48)&&
          field(c,0x4820,0x10000),"unmapped GDT blocks LLDT and preserves null guest LDTR");
    check(!hv_vm_unmap(0x1000,4*4096)&&!hv_vm_unmap(0x8000,4*4096),"unmap system guest");
    free(mem);check(!hv_vcpu_destroy(c)&&!hv_vm_destroy(),"system guest cleanup");
}
int main(int argc,char **argv){
    int fast=0;size_t fastBytes=sizeof(fast);
    if(sysctlbyname("debug.amdhv.fast",&fast,&fastBytes,nullptr,0))return 2;
    fastMode=fast!=0;
    if(argc==2&&!strcmp(argv[1],"--diagnostic-normalize-segments"))diagnosticTrRepair=true;
    else if(argc!=1)return 2;
    setvbuf(stdout,nullptr,_IONBF,0);
    if(diagnosticTrRepair)puts("DIAGNOSTIC ONLY: userspace segment normalization enabled; does not validate unmodified kext");
    mode(false);mode(true);return failures?1:0;
}
