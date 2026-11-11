#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <sys/sysctl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
static unsigned failures;
static void check(bool ok,const char *s){printf("%s: %s\n",ok?"PASS":"FAIL",s);failures+=!ok;}
static bool field(hv_vcpuid_t c,unsigned f,uint64_t expected){uint64_t v=~0ull;auto r=hv_vmx_vcpu_read_vmcs(c,f,&v);
    if(r||v!=expected)printf("VMCS%x=%llx expected=%llx r=%x\n",f,v,expected,r);return !r&&v==expected;}
static bool reg(hv_vcpuid_t c,hv_x86_reg_t r,uint64_t expected){uint64_t v=~0ull;auto e=hv_vcpu_read_register(c,r,&v);
    if(e||v!=expected)printf("reg%d=%llx expected=%llx r=%x\n",r,v,expected,e);return !e&&v==expected;}
int main(){
    setvbuf(stdout,nullptr,_IONBF,0);int fast=0;size_t n=sizeof(fast);
    if(sysctlbyname("debug.amdhv.fast",&fast,&n,nullptr,0)||fast!=1){puts("Requires fast mode after timer gate");return 2;}
    if(hv_vm_create(HV_VM_DEFAULT))return 2;
    hv_vcpuid_t c;if(hv_vcpu_create(&c,HV_VCPU_DEFAULT)){hv_vm_destroy();return 2;}
    void *mem=nullptr;if(posix_memalign(&mem,4096,9*4096)){hv_vcpu_destroy(c);hv_vm_destroy();return 2;}
    memset(mem,0,9*4096);auto b=static_cast<unsigned char*>(mem);auto code=b+4*4096;auto t=static_cast<uint64_t*>(mem);
    constexpr uint64_t va=0xffff800000000000ull,magic=0x123456789abcdef0ull;
    t[0]=t[256]=0x2003;t[512]=0x3003;t[1024]=0x4003;t[1536+4]=0x4003;
    for(unsigned i=8;i<12;++i)t[1536+i]=i*4096+3;
    memcpy(code+0x4000,&magic,8);
    bool ok=!hv_vm_map(mem,0x1000,4*4096,HV_MEMORY_READ|HV_MEMORY_WRITE)&&
        !hv_vm_map(code,0x8000,5*4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC);
    const struct {unsigned f;uint64_t v;} fields[]={{0x6800,0x80010033},{0x6802,0x1000},{0x6804,0x20},{0x2806,0xd00},
        {0x6816,va+0xa000},{0x4810,23},{0x6818,va+0xb000},{0x4812,4095}};
    for(auto f:fields)ok &= !hv_vmx_vcpu_write_vmcs(c,f.f,f.v);
    for(unsigned i=0;i<6;++i){
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x800+i*2,i==1?8:16);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x4800+i*2,0xffffffff);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x4814+i*2,i==1?0xa09b:0xc093);
    }
    uint64_t gdt[]={0,0x00af9b000000ffffull,0x00cf93000000ffffull};memcpy(code+0x2000,gdt,sizeof(gdt));
    auto gate=[&](unsigned vector,uint64_t handler){auto p=code+0x3000+vector*16;
        memset(p,0,16);p[0]=handler;p[1]=handler>>8;p[2]=8;p[5]=0x8e;
        p[6]=handler>>16;p[7]=handler>>24;for(unsigned i=0;i<4;++i)p[8+i]=handler>>(32+i*8);};
    gate(14,va+0x8100);gate(6,va+0x8180);gate(13,va+0x8200);
    // #PF: capture CR2/error/RIP, install a guest PTE, flush guest translations,
    // preserve the faulting operand and retry the read after dropping error code.
    const unsigned char pf[]={0x0f,0x20,0xd2,0x4c,0x8b,0x04,0x24,0x4c,0x8b,0x4c,0x24,8,
        0x49,0xba,0x60,0x40,0,0,0,0x80,0xff,0xff,0x49,0xc7,0x02,3,0xc0,0,0,
        0x49,0x89,0xc3,0x0f,0x20,0xd8,0x0f,0x22,0xd8,0x4c,0x89,0xd8,0x48,0x83,0xc4,8,0x48,0xcf};
    memcpy(code+0x100,pf,sizeof(pf));
    const unsigned char ud[]={0x4c,0x8b,0x0c,0x24,0x48,0x83,0x04,0x24,2,0x48,0xcf};
    memcpy(code+0x180,ud,sizeof(ud));
    const unsigned char gp[]={0x4c,0x8b,0x04,0x24,0x4c,0x8b,0x4c,0x24,8,
        0x48,0x83,0x44,0x24,8,2,0x48,0x83,0xc4,8,0x48,0xcf};
    memcpy(code+0x200,gp,sizeof(gp));check(ok,"configure high guest IDT, page tables and synchronous fault handlers");
    auto prepare=[&](std::initializer_list<unsigned char> bytes){unsigned i=0;for(auto v:bytes)code[i++]=v;
        hv_vcpu_write_register(c,HV_X86_RIP,va+0x8000);hv_vcpu_write_register(c,HV_X86_RSP,va+0x9087);
        hv_vcpu_write_register(c,HV_X86_RFLAGS,0x246);hv_vmx_vcpu_write_vmcs(c,0x4016,0);hv_vmx_vcpu_write_vmcs(c,0x4824,0);};
    auto run=[&]() -> hv_return_t {for(unsigned i=0;i<256;++i){auto r=hv_vcpu_run(c);if(r)return r;
        uint64_t reason=0;if(hv_vmx_vcpu_read_vmcs(c,VMCS_RO_EXIT_REASON,&reason))return HV_ERROR;
        if(reason!=1)return HV_SUCCESS;}return HV_ERROR;};
    auto pageFault=[&](){prepare({0x48,0xb8,0,0xc0,0,0,0,0x80,0xff,0xff,0x48,0x8b,0x18,0xf4});};
    pageFault();check(run()==0&&field(c,VMCS_RO_EXIT_REASON,12)&&reg(c,HV_X86_RBX,magic)&&reg(c,HV_X86_RDX,va+0xc000)&&
        reg(c,HV_X86_R8,0)&&reg(c,HV_X86_R9,va+0x800a)&&reg(c,HV_X86_RSP,va+0x9087),
        "native PF handler updates guest PTE/CR3 then IRET retries the original memory read");
    prepare({0x0f,0x0b,0xf4});check(run()==0&&field(c,VMCS_RO_EXIT_REASON,12)&&reg(c,HV_X86_R9,va+0x8000)&&reg(c,HV_X86_RSP,va+0x9087),
        "native UD handler receives a frame without error code and skips UD2");
    prepare({0xb8,0x30,0,0,0,0x8e,0xd8,0xf4});check(run()==0&&field(c,VMCS_RO_EXIT_REASON,12)&&reg(c,HV_X86_R8,0x30)&&
        reg(c,HV_X86_R9,va+0x8005)&&reg(c,HV_X86_RSP,va+0x9087),"native GP handler receives selector error code and restores stack");
    t[1536+12]=0;hv_vmx_vcpu_write_vmcs(c,0x4004,1u<<14);hv_vcpu_write_register(c,HV_X86_CR2,0xdead);
    pageFault();check(run()==0&&field(c,VMCS_RO_EXIT_REASON,0)&&field(c,0x4404,0x80000b0e)&&field(c,0x4406,0)&&
        field(c,0x6400,va+0xc000)&&reg(c,HV_X86_CR2,0xdead)&&reg(c,HV_X86_RIP,va+0x800a),
        "PF bitmap exits as VMX0 with error/linear address and leaves CR2 unchanged");
    hv_vcpu_write_register(c,HV_X86_CR2,va+0xc000);hv_vmx_vcpu_write_vmcs(c,0x4016,0x80000b0e);hv_vmx_vcpu_write_vmcs(c,0x4018,0);
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,12)&&reg(c,HV_X86_RBX,magic)&&field(c,0x4016,0),
        "VMCS PF injection reaches guest handler once and retries the faulting read");
    hv_vmx_vcpu_write_vmcs(c,0x4008,0xffffffff);t[1536+12]=0;pageFault();
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,12)&&reg(c,HV_X86_RBX,magic),
        "constant PF mask/match inversion delivers natively when bitmap bit is set");
    hv_vmx_vcpu_write_vmcs(c,0x4004,0);t[1536+12]=0;pageFault();
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,0)&&field(c,0x4404,0x80000b0e),
        "constant PF mask/match inversion exits when bitmap bit is clear");
    hv_vmx_vcpu_write_vmcs(c,0x4008,0);
    hv_vmx_vcpu_write_vmcs(c,0x4004,1u<<6);prepare({0x0f,0x0b,0xf4});
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,0)&&field(c,0x4404,0x80000306)&&field(c,0x4406,0)&&reg(c,HV_X86_RIP,va+0x8000),
        "UD bitmap exits as VMX0 without an error code or RIP advance");
    hv_vmx_vcpu_write_vmcs(c,0x4016,0x80000306);
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,12)&&reg(c,HV_X86_R9,va+0x8000),
        "VMCS UD injection enters the guest without pushing a synthetic error code");
    hv_vmx_vcpu_write_vmcs(c,0x4004,1u<<13);prepare({0xb8,0x30,0,0,0,0x8e,0xd8,0xf4});
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,0)&&field(c,0x4404,0x80000b0d)&&field(c,0x4406,0x30),
        "GP bitmap forwards the nonzero selector error code");
    hv_vmx_vcpu_write_vmcs(c,0x4016,0x80000b0d);hv_vmx_vcpu_write_vmcs(c,0x4018,0x30);
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,12)&&reg(c,HV_X86_R8,0x30),
        "VMCS GP injection carries its nonzero error code through EVENTINJ");
    hv_vmx_vcpu_write_vmcs(c,0x4004,0);hv_vmx_vcpu_write_vmcs(c,0x4006,1);prepare({0x0f,0x0b});
    check(hv_vcpu_run(c)==HV_UNSUPPORTED,"nonconstant PF error mask fails closed before entry");hv_vmx_vcpu_write_vmcs(c,0x4006,0);
    t[1536+13]=0xd003;hv_vmx_vcpu_write_vmcs(c,0x6818,va+0xd000);prepare({0x0f,0x0b,0xf4});
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,48)&&field(c,0x4408,0x80000306),
        "NPT fault during UD gate lookup reports interrupted exception delivery separately");
    hv_vmx_vcpu_write_vmcs(c,0x6818,va+0xb000);
    // Linux uses INT3 to self-test its trap path. Native delivery must save the
    // post-INT3 RIP, unlike the faulting RIP saved for UD2.
    gate(3,va+0x8280);
    const unsigned char bp[]={0x4c,0x8b,0x0c,0x24,0x48,0xcf};
    memcpy(code+0x280,bp,sizeof(bp));prepare({0xcc,0xf4});
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,12)&&reg(c,HV_X86_R9,va+0x8001)&&reg(c,HV_X86_RSP,va+0x9087),
        "native INT3 trap saves post-instruction RIP and IRET resumes at HLT");
    hv_vmx_vcpu_write_vmcs(c,0x4004,8);prepare({0xcc,0xf4});
    check(run()==HV_UNSUPPORTED,"intercepted software trap fails closed until VMX type6 ABI is supported");
    hv_vmx_vcpu_write_vmcs(c,0x4004,0);
    // Native CR4 feature group. Keep every code/IDT/stack PTE supervisor-only;
    // selectively mark data/target page12 user-accessible for SMEP/SMAP checks.
    bool managed=true;
    for(uint32_t address : {0xc0000100u,0xc0000101u,0xc0000102u}) {
        managed &= !hv_vcpu_enable_managed_msr(c,address,true);
        managed &= !hv_vcpu_set_msr_access(c,address,HV_MSR_READ|HV_MSR_WRITE);
    }
    managed &= !hv_vcpu_write_msr(c,0xc0000102,va+0xa000);
    t[1536+12]=0xc003;uint64_t gsMagic=0xfedcba9876543210ull;
    memcpy(code+0x4000,&magic,8);memcpy(code+0x1000,&gsMagic,8);
    prepare({0xb8,0x20,8,0x31,0,0x0f,0x22,0xe0, // CR4=310820
        0x41,0x0f,0x20,0xe3, // mov CR4,r11
        0x48,0xb8,0,0xc0,0,0,0,0x80,0xff,0xff,0xf3,0x48,0x0f,0xae,0xd0,
        0x48,0xb8,0,0x90,0,0,0,0x80,0xff,0xff,0xf3,0x48,0x0f,0xae,0xd8,
        0xf3,0x48,0x0f,0xae,0xc3,0xf3,0x48,0x0f,0xae,0xc9,
        0x64,0x48,0x8b,0x14,0x25,0,0,0,0, // FS:[0] -> rdx
        0x65,0x4c,0x8b,0x04,0x25,0,0,0,0, // GS:[0] -> r8
        0x0f,0x01,0xf8,0xf3,0x49,0x0f,0xae,0xc9, // swapgs; rdgsbase r9
        0x0f,0x01,0xf8,0xf3,0x49,0x0f,0xae,0xca,0xf4});
    check(managed&&run()==0&&field(c,VMCS_RO_EXIT_REASON,12)&&reg(c,HV_X86_R11,0x310820)&&
        reg(c,HV_X86_RBX,va+0xc000)&&reg(c,HV_X86_RCX,va+0x9000)&&reg(c,HV_X86_RDX,magic)&&
        reg(c,HV_X86_R8,gsMagic)&&reg(c,HV_X86_R9,va+0xa000)&&reg(c,HV_X86_R10,va+0x9000)&&
        field(c,0x680e,va+0xc000)&&field(c,0x6810,va+0x9000),
        "guest enables FSGSBASE/SMEP/SMAP/UMIP, uses FS/GS and SWAPGS without changing host bases");
    prepare({0xf3,0x48,0x0f,0xae,0xc3,0xf3,0x48,0x0f,0xae,0xc9,
        0xb9,0,1,0,0xc0,0x0f,0x32,0xf4}); // managed FS RDMSR after base read
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,12)&&reg(c,HV_X86_RBX,va+0xc000)&&
        reg(c,HV_X86_RAX,0xc000)&&reg(c,HV_X86_RDX,0xffff8000)&&field(c,0x6804,0x310820),
        "native bases and CR4 survive re-entry and managed FS MSR readback");
    hv_vmx_vcpu_write_vmcs(c,0x4004,1u<<6);hv_vmx_vcpu_write_vmcs(c,0x6804,0x300820);
    prepare({0xf3,0x48,0x0f,0xae,0xd0,0xf4});
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,0)&&field(c,0x4404,0x80000306)&&field(c,0x680e,va+0xc000),
        "WRFSBASE with FSGSBASE disabled raises guest UD without changing FS");
    hv_vmx_vcpu_write_vmcs(c,0x4004,1u<<13);hv_vmx_vcpu_write_vmcs(c,0x6804,0x310820);
    prepare({0x48,0xb8,0,0,0,0,0,0x80,0,0,0xf3,0x48,0x0f,0xae,0xd0,0xf4});
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,0)&&field(c,0x4404,0x80000b0d)&&field(c,0x4406,0)&&field(c,0x680e,va+0xc000),
        "noncanonical native FS base raises guest GP and preserves the previous base");
    // UMIP affects CPL>0 only. This gate covers permitted CPL0 SGDT; ring3 is
    // still outside the backend's validated modes.
    hv_vmx_vcpu_write_vmcs(c,0x4004,0);prepare({0x48,0xb8,0,0x90,0,0,0,0x80,0xff,0xff,0x0f,0x01,0,0xf4});
    uint16_t savedLimit=0;uint64_t savedBase=0;auto umipResult=run();
    memcpy(&savedLimit,code+0x1000,2);memcpy(&savedBase,code+0x1002,8);
    check(umipResult==0&&field(c,VMCS_RO_EXIT_REASON,12)&&savedLimit==23&&savedBase==va+0xa000,
        "UMIP preserves native CPL0 SGDT behavior");
    t[0]=t[256]=0x2007;t[512]=0x3007;t[1024]=0x4007;t[1536+12]=0xc007;
    hv_vmx_vcpu_write_vmcs(c,0x4004,1u<<14);hv_vmx_vcpu_write_vmcs(c,0x6804,0x310820);
    code[0x4000]=0xf4;prepare({0x48,0xb8,0,0xc0,0,0,0,0x80,0xff,0xff,0xff,0xe0});
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,0)&&field(c,0x4404,0x80000b0e)&&field(c,0x4406,0x11)&&
        field(c,0x6400,va+0xc000)&&reg(c,HV_X86_RIP,va+0xc000),
        "SMEP blocks CPL0 execution of a user PTE and reports a guest fetch PF");
    memcpy(code+0x4000,&magic,8);pageFault();
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,0)&&field(c,0x4406,1)&&field(c,0x6400,va+0xc000),
        "SMAP blocks CPL0 data read of a user PTE while AC is clear");
    prepare({0x0f,0x01,0xcb,0xf4}); // STAC; HLT, force an entry boundary with AC1
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,12)&&field(c,0x6820,0x40246),
        "STAC publishes guest AC across an exit without changing host flags");
    prepare({0x48,0xb8,0,0xc0,0,0,0,0x80,0xff,0xff,0x48,0x8b,0x18,0x0f,0x01,0xca,0xf4});
    hv_vcpu_write_register(c,HV_X86_RFLAGS,0x40246);
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,12)&&reg(c,HV_X86_RBX,magic)&&field(c,0x6820,0x246),
        "AC survives re-entry, permits the SMAP user-data read, then CLAC clears it");
    // Mask/shadow handling retains priority over native CR4 support.
    hv_vmx_vcpu_write_vmcs(c,0x6804,0x20);hv_vmx_vcpu_write_vmcs(c,0x6002,0x10000);hv_vmx_vcpu_write_vmcs(c,0x6006,0);
    prepare({0xb8,0x20,0,1,0,0x0f,0x22,0xe0,0xf4});
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,28)&&field(c,0x6804,0x20)&&reg(c,HV_X86_RIP,va+0x8005),
        "native CR4 feature write still exits for a VMX-owned bit before committing it");
    hv_vmx_vcpu_write_vmcs(c,0x6002,0);hv_vmx_vcpu_write_vmcs(c,0x6804,0x40020);prepare({0xf4});
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,12),"fast code64 accepts OSXSAVE with the bounded extended-state switch");
    hv_vmx_vcpu_write_vmcs(c,0x6804,0x20);hv_vmx_vcpu_write_vmcs(c,0x4004,0);
    t[0]=t[256]=0x2003;t[512]=0x3003;t[1024]=0x4003;t[1536+12]=0xc003;
    memset(code+0x3000,0,4096);
    prepare({0x0f,0x0b});
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,2),"unserviceable guest faults report VMX triple-fault exit without stopping the host");
    check(!hv_vm_unmap(0x1000,4*4096)&&!hv_vm_unmap(0x8000,5*4096),"unmap fault guest");
    free(mem);check(!hv_vcpu_destroy(c)&&!hv_vm_destroy(),"fault guest cleanup");return failures?1:0;
}
