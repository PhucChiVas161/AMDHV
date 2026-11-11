#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <sys/sysctl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
static unsigned failures;
static void check(bool ok,const char *label){printf("%s: %s\n",ok?"PASS":"FAIL",label);failures+=!ok;}
static bool field(hv_vcpuid_t c,unsigned f,uint64_t expected){uint64_t got=~0ull;auto r=hv_vmx_vcpu_read_vmcs(c,f,&got);
    if(r||got!=expected)printf("VMCS%x=%llx expected=%llx r=%x\n",f,got,expected,r);return !r&&got==expected;}
static bool reg(hv_vcpuid_t c,hv_x86_reg_t r,uint64_t expected){uint64_t got=~0ull;auto e=hv_vcpu_read_register(c,r,&got);
    if(e||got!=expected)printf("reg%u=%llx expected=%llx r=%x\n",unsigned(r),got,expected,e);return !e&&got==expected;}
int main(){
    setvbuf(stdout,nullptr,_IONBF,0);int fast=0;size_t size=sizeof(fast);
    if(sysctlbyname("debug.amdhv.fast",&fast,&size,nullptr,0)||fast!=1){puts("Requires fast mode after the timer/state gates");return 2;}
    if(hv_vm_create(HV_VM_DEFAULT)){check(false,"create CPL3 VM");return 2;}
    hv_vcpuid_t c;if(hv_vcpu_create(&c,HV_VCPU_DEFAULT)){hv_vm_destroy();return 2;}
    void *mem=nullptr;if(posix_memalign(&mem,4096,12*4096)){hv_vcpu_destroy(c);hv_vm_destroy();return 2;}
    memset(mem,0,12*4096);auto t=static_cast<uint64_t*>(mem);auto ram=static_cast<unsigned char*>(mem)+4*4096;
    auto user=ram+0x2000;auto tss=ram+0x6000;
    constexpr uint64_t k=0xffff800000000000ull,u=0x100000000ull;
    t[0]=t[256]=0x2007;t[512]=t[516]=0x3007;t[1024]=0x4007;
    for(unsigned i=8;i<16;++i)t[1536+i]=i*4096+3;
    t[1536+10]|=4;t[1536+11]|=4;t[1536+16]=0x10007;t[1536+17]=0x11003;
    bool ok=!hv_vm_map(mem,0x1000,4*4096,HV_MEMORY_READ|HV_MEMORY_WRITE)&&
        !hv_vm_map(ram,0x8000,8*4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC);
    auto q=[](unsigned char *p,uint64_t value){memcpy(p,&value,8);};
    uint64_t gdt[]={0,0x00af9b000000ffffull,0x00cf93000000ffffull,0,
        0x00cffb000000ffffull,0x00cff3000000ffffull,0x00affb000000ffffull,0,0};
    uint64_t base=k+0xe000;gdt[7]=0xfffull|((base&0xffffff)<<16)|(0x8bull<<40)|(((base>>24)&255)<<56);gdt[8]=base>>32;
    memcpy(ram+0x4000,gdt,sizeof(gdt));q(tss+4,k+0x9087);q(tss+0x24,k+0x9187);
    tss[0x66]=0x80;memset(tss+0x80,255,4096-0x80);tss[0x90]=0xfe; // only byte port80 allowed
    auto gate=[&](unsigned vector,uint64_t handler,unsigned access=0x8e,unsigned ist=0){auto p=ram+0x5000+vector*16;
        memset(p,0,16);p[0]=handler;p[1]=handler>>8;p[2]=8;p[4]=ist;p[5]=access;
        p[6]=handler>>16;p[7]=handler>>24;for(unsigned i=0;i<4;++i)p[8+i]=handler>>(32+i*8);};
    gate(13,k+0x8200);gate(14,k+0x8200);gate(6,k+0x8300);
    gate(64,k+0x8400);gate(0x80,k+0x8400,0xee);gate(0x81,k+0x8400);gate(3,k+0x8400,0xee);
    // Fault handler captures the real privilege frame, skips a caller-supplied
    // instruction length, removes the error code and returns to user CPUID.
    const unsigned char fault[]={0x0f,0x20,0xd2,0x4c,0x8b,0x04,0x24,0x4c,0x8b,0x4c,0x24,8,
        0x4c,0x8b,0x54,0x24,16,0x4c,0x8b,0x64,0x24,32,0x4c,0x8b,0x6c,0x24,40,
        0x49,0x89,0xe6,0x4c,0x01,0x7c,0x24,8,0x48,0x83,0xc4,8,0x48,0xcf};
    memcpy(ram+0x200,fault,sizeof(fault));
    const unsigned char noerror[]={0x4c,0x8b,0x0c,0x24,0x4c,0x8b,0x54,0x24,8,
        0x4c,0x8b,0x64,0x24,24,0x4c,0x8b,0x6c,0x24,32,0x49,0x89,0xe6,
        0x4c,0x01,0x3c,0x24,0x48,0xcf};
    memcpy(ram+0x300,noerror,sizeof(noerror));
    const unsigned char irq[]={0x4c,0x8b,0x0c,0x24,0x4c,0x8b,0x54,0x24,8,
        0x4c,0x8b,0x64,0x24,24,0x4c,0x8b,0x6c,0x24,32,0x49,0x89,0xe6,0x0f,0xa2,0x48,0xcf};
    memcpy(ram+0x400,irq,sizeof(irq));
    const unsigned char syscall[]={0x49,0x89,0xc8,0x4d,0x89,0xd9,0x49,0x89,0xe2,
        0x48,0xbc,0x87,0x90,0,0,0,0x80,0xff,0xff,0x0f,0x01,0xf8,
        0xf3,0x49,0x0f,0xae,0xc4,0xf3,0x49,0x0f,0xae,0xcd,
        0x0f,0xa2,0x4c,0x89,0xd4,0x0f,0x01,0xf8,0x48,0x0f,0x07};
    memcpy(ram+0x100,syscall,sizeof(syscall));
    const struct{unsigned f;uint64_t v;} fields[]={{0x6800,0x80010033},{0x6802,0x1000},{0x6804,0x110620},
        {0x2806,0xd01},{0x6816,k+0xc000},{0x4810,sizeof(gdt)-1},{0x6818,k+0xd000},{0x4812,4095},
        {0x80e,0x38},{0x6814,k+0xe000},{0x480e,0xfff},{0x4822,0x8b}};
    for(auto f:fields)ok &= !hv_vmx_vcpu_write_vmcs(c,f.f,f.v);
    for(unsigned i=0;i<6;++i){ok &= !hv_vmx_vcpu_write_vmcs(c,0x800+i*2,i==1?8:16);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x4800+i*2,0xffffffff);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x4814+i*2,i==1?0xa09b:0xc093);}
    check(ok,"configure GDT, user PTEs, kernel-only tables/stacks, TSS RSP0/IST and I/O bitmap");
    auto run=[&]() -> hv_return_t{for(unsigned i=0;i<256;++i){auto r=hv_vcpu_run(c);if(r)return r;
        uint64_t reason=0;hv_vmx_vcpu_read_vmcs(c,0x4402,&reason);if(reason!=1)return HV_SUCCESS;}return HV_ERROR;};
    auto advance=[&](){uint64_t rip=0;hv_vcpu_read_register(c,HV_X86_RIP,&rip);hv_vcpu_write_register(c,HV_X86_RIP,rip+2);};
    // Enter user mode through a real outer-privilege IRETQ before exercising
    // restored API contexts, not just by writing a ring3 selector into VMCS.
    q(ram+0x1200,u+0xa000);q(ram+0x1208,0x33);q(ram+0x1210,0x246);q(ram+0x1218,u+0xb087);q(ram+0x1220,0x2b);
    ram[0]=0x48;ram[1]=0xcf;user[0]=0x0f;user[1]=0xa2;
    hv_vcpu_write_register(c,HV_X86_RIP,k+0x8000);hv_vcpu_write_register(c,HV_X86_RSP,k+0x9200);
    check(run()==0&&field(c,0x4402,10)&&field(c,0x802,0x33)&&field(c,0x804,0x2b)&&reg(c,HV_X86_RSP,u+0xb087),
        "native IRETQ enters CPL3 with the requested unaligned user stack");
    auto prepare=[&](std::initializer_list<unsigned char> bytes,unsigned skip=0){unsigned i=0;for(auto v:bytes)user[i++]=v;
        hv_vcpu_write_register(c,HV_X86_RIP,u+0xa000);hv_vcpu_write_register(c,HV_X86_RSP,u+0xb087);
        hv_vcpu_write_register(c,HV_X86_RFLAGS,0x246);hv_vmx_vcpu_write_vmcs(c,0x4824,0);hv_vmx_vcpu_write_vmcs(c,0x4016,0);
        for(auto r:{HV_X86_R8,HV_X86_R9,HV_X86_R10,HV_X86_R12,HV_X86_R13,HV_X86_R14})hv_vcpu_write_register(c,r,~0ull);
        hv_vcpu_write_register(c,HV_X86_R15,skip);};
    auto frame=[&](uint64_t error){return reg(c,HV_X86_R8,error)&&reg(c,HV_X86_R9,u+0xa000)&&reg(c,HV_X86_R10,0x33)&&
        reg(c,HV_X86_R12,u+0xb087)&&reg(c,HV_X86_R13,0x2b)&&reg(c,HV_X86_R14,k+0x9050);};
    check(run()==0&&field(c,0x4402,10),"published CPL3 context re-enters without being restored as CPL0");
    for(uint32_t address:{0xc0000081u,0xc0000082u,0xc0000084u,0xc0000100u,0xc0000101u,0xc0000102u})
        ok &= !hv_vcpu_enable_managed_msr(c,address,true);
    // AMD SYSRET does not force SS.RPL to3; STAR's user selector must include it.
    ok &= !hv_vcpu_write_msr(c,0xc0000081,0x0023000800000000ull);
    ok &= !hv_vcpu_write_msr(c,0xc0000082,k+0x8100);ok &= !hv_vcpu_write_msr(c,0xc0000084,0x300);
    ok &= !hv_vcpu_write_msr(c,0xc0000100,u+0xb200);ok &= !hv_vcpu_write_msr(c,0xc0000101,u+0xb300);
    ok &= !hv_vcpu_write_msr(c,0xc0000102,k+0xf000);
    constexpr uint64_t magic=0x123456789abcdef0ull;q(ram+0x3200,magic);
    prepare({0x64,0x48,0x8b,4,0x25,0,0,0,0,0x0f,0xa2});
    check(ok&&run()==0&&field(c,0x4402,10)&&reg(c,HV_X86_RAX,magic),"CPL3 reads its FS-based TLS through a user page");
    prepare({0x48,0xba,0,0xb4,0,0,1,0,0,0,0xf3,0x48,0x0f,0xae,0xd2,0x0f,0xa2});
    check(run()==0&&field(c,0x4402,10)&&field(c,0x680e,u+0xb400),"user WRFSBASE changes only the saved guest TLS base");
    prepare({0x0f,0x05,0x0f,0xa2});
    check(run()==0&&field(c,0x4402,10)&&field(c,0x802,8)&&field(c,0x6820,0x46)&&reg(c,HV_X86_RSP,k+0x9087)&&
        reg(c,HV_X86_R8,u+0xa002)&&reg(c,HV_X86_R9,0x246)&&reg(c,HV_X86_R10,u+0xb087)&&
        reg(c,HV_X86_R12,u+0xb400)&&reg(c,HV_X86_R13,k+0xf000),"SYSCALL enters CPL0, masks IF, switches kernel stack/GS and retains user RCX/R11");
    advance();
    check(run()==0&&field(c,0x4402,10)&&field(c,0x802,0x33)&&field(c,0x804,0x2b)&&
        reg(c,HV_X86_RIP,u+0xa002)&&reg(c,HV_X86_RSP,u+0xb087)&&field(c,0x6810,u+0xb300),
        "kernel re-entry and SYSRETQ restore user CPL/stack/GS with normalized SS.DPL");
    check(run()==0&&field(c,0x4402,10),"SYSRET-published user caches survive another run");
    auto gp=[&](std::initializer_list<unsigned char> bytes,unsigned length,const char *label){prepare(bytes,length);
        check(run()==0&&field(c,0x4402,10)&&frame(0)&&field(c,0x802,0x33),label);};
    gp({0x0f,0x20,0xd8,0x0f,0xa2},3,"user MOV CR3 faults before privileged emulation and IRET resumes user code");
    gp({0x0f,0x21,0xc0,0x0f,0xa2},3,"user MOV DR0 faults before guest debug-state access");
    hv_vcpu_write_register(c,HV_X86_RCX,0xc0000100);
    gp({0x0f,0x32,0x0f,0xa2},2,"user RDMSR faults before the managed-MSR handler");
    hv_vcpu_write_register(c,HV_X86_RAX,0x1234);hv_vcpu_write_register(c,HV_X86_RDX,0);
    gp({0x0f,0x30,0x0f,0xa2},2,"user WRMSR faults without changing guest or host managed state");
    check(field(c,0x680e,u+0xb400),"denied user WRMSR leaves the guest FS base unchanged");
    hv_vcpu_write_register(c,HV_X86_RCX,0x49);hv_vcpu_write_register(c,HV_X86_RAX,1);hv_vcpu_write_register(c,HV_X86_RDX,0);
    gp({0x0f,0x30,0x0f,0xa2},2,"CPL3 PRED_CMD cannot invoke a physical speculation barrier");
    hv_vcpu_write_register(c,HV_X86_RCX,0x48);
    gp({0x0f,0x32,0x0f,0xa2},2,"CPL3 SPEC_CTRL cannot read the guest or host speculation controls");
    gp({0xf4,0x0f,0xa2},1,"user HLT raises guest GP rather than a VMX HLT exit");
    gp({0xfa,0x0f,0xa2},1,"user CLI cannot bypass IOPL0");
    gp({0xfb,0x0f,0xa2},1,"user STI cannot bypass IOPL0");
    gp({0x0f,0x01,0x38,0x0f,0xa2},3,"user INVLPG raises GP before any TLB emulation");
    gp({0x0f,0x09,0x0f,0xa2},2,"user WBINVD raises GP before coherent cache emulation");
    gp({0x0f,0x08,0x0f,0xa2},2,"user INVD raises GP rather than a privileged VMX13 exit");
    hv_vmx_vcpu_write_vmcs(c,0x6804,0x150620);
    hv_vcpu_write_register(c,HV_X86_RAX,7);hv_vcpu_write_register(c,HV_X86_RCX,0);hv_vcpu_write_register(c,HV_X86_RDX,0);
    gp({0x0f,0x01,0xd1,0x0f,0xa2},3,"intercept-before-GP XSETBV at CPL3 injects GP without changing XCR0");
    check(reg(c,static_cast<hv_x86_reg_t>(50),1),"denied user XSETBV did not commit its otherwise valid value");
    hv_vmx_vcpu_write_vmcs(c,0x4004,1u<<13);prepare({0x0f,0x01,0xd1,0x0f,0xa2});
    check(run()==0&&field(c,0x4402,0)&&field(c,0x4404,0x80000b0d)&&field(c,0x4406,0)&&reg(c,HV_X86_RIP,u+0xa000),
        "CPL3 XSETBV honors the VMX GP bitmap without advancing the instruction");
    hv_vmx_vcpu_write_vmcs(c,0x4004,0);hv_vmx_vcpu_write_vmcs(c,0x6804,0x110e20);
    gp({0x0f,0x01,0xe0,0x0f,0xa2},3,"UMIP raises user GP for SMSW");
    hv_vmx_vcpu_write_vmcs(c,0x6804,0x110620);
    gp({0xe4,0x81,0x0f,0xa2},2,"denied user port access checks the guest TSS bitmap before IOIO exit");
    prepare({0xe4,0x80,0x0f,0xa2});
    check(run()==0&&field(c,0x4402,30)&&field(c,0x6400,0x800048)&&field(c,0x440c,2),
        "permitted user port reaches VMX IO exit and its user code is fetched with SMEP enabled");
    advance();check(run()==0&&field(c,0x4402,10),"authorized user IO resumes after userspace emulation");
    hv_vmx_vcpu_write_vmcs(c,0x4004,1u<<14);
    prepare({0x48,0xb8,0,0xf0,0,0,0,0x80,0xff,0xff,0x48,0x8b,0x18,0x0f,0xa2});
    check(run()==0&&field(c,0x4402,0)&&field(c,0x4406,5)&&field(c,0x6400,k+0xf000),
        "restored CPL3 cannot read a supervisor page; PF retains the user-access error bit");
    hv_vmx_vcpu_write_vmcs(c,0x4004,0);
    prepare({0x48,0xb8,0,0,1,0,1,0,0,0,0x48,0x8b,0x18,0x0f,0xa2});
    check(run()==0&&field(c,0x4402,48)&&field(c,0x2400,0x10000),"present user PTE cannot bypass missing NPT ownership");
    auto deliver=[&](std::initializer_list<unsigned char> code,uint64_t returnRip,bool external,uint64_t stack,const char *label){
        prepare(code);if(external)hv_vmx_vcpu_write_vmcs(c,0x4016,0x80000040);
        check(run()==0&&field(c,0x4402,10)&&field(c,0x802,8)&&reg(c,HV_X86_R9,returnRip)&&
            reg(c,HV_X86_R10,0x33)&&reg(c,HV_X86_R12,u+0xb087)&&reg(c,HV_X86_R13,0x2b)&&reg(c,HV_X86_R14,stack),label);
        advance();check(run()==0&&field(c,0x4402,10)&&field(c,0x802,0x33)&&reg(c,HV_X86_RIP,returnRip),
            "kernel event frame survives re-entry and IRETQ returns to the original user stack");};
    deliver({0x0f,0xa2},u+0xa000,true,k+0x9058,"external IRQ uses TSS RSP0 and records a real outer-privilege frame");
    gate(64,k+0x8400,0x8e,1);
    deliver({0x0f,0xa2},u+0xa000,true,k+0x9158,"user IRQ with IST1 uses the guest IST stack");
    gate(64,k+0x8400);
    deliver({0xcd,0x80,0x0f,0xa2},u+0xa002,false,k+0x9058,"native INT80 honors a DPL3 gate and saves post-instruction RIP");
    prepare({0xcd,0x81,0x0f,0xa2},2);
    check(run()==0&&field(c,0x4402,10)&&frame(0x40a),"INT81 cannot invoke a DPL0 gate from CPL3");
    deliver({0xcc,0x0f,0xa2},u+0xa001,false,k+0x9058,"user INT3 enters its DPL3 gate with post-instruction RIP");
    // Last case intentionally interrupts delivery; do not retry its incomplete
    // guest event. Teardown must remain possible after a kernel-stack NPF.
    q(tss+4,k+0x11087);prepare({0x0f,0xa2});hv_vmx_vcpu_write_vmcs(c,0x4016,0x80000040);
    check(run()==0&&field(c,0x4402,48)&&field(c,0x4408,0x80000040),"NPT blocks an unowned privilege-stack page and reports interrupted IRQ delivery");
    check(!hv_vcpu_destroy(c),"destroy CPL3 vCPU after an interrupted event");
    check(!hv_vm_unmap(0x1000,4*4096)&&!hv_vm_unmap(0x8000,8*4096),"unmap CPL3 guest");
    free(mem);check(!hv_vm_destroy(),"CPL3 guest cleanup");return failures?1:0;
}
