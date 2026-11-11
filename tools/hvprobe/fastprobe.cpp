#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <chrono>
#include <thread>
#include <atomic>
static unsigned failures;
static void check(bool ok,const char *label){printf("%s: %s\n",ok?"PASS":"FAIL",label);failures+=!ok;}
static bool field(hv_vcpuid_t c,unsigned f,uint64_t expected){uint64_t v=~0ull;auto r=hv_vmx_vcpu_read_vmcs(c,f,&v);if(r||v!=expected)printf("VMCS%x=%llx expected=%llx r=%x\n",f,v,expected,r);return !r&&v==expected;}
static bool reg(hv_vcpuid_t c,hv_x86_reg_t r,uint64_t expected){uint64_t v=~0ull;auto e=hv_vcpu_read_register(c,r,&v);if(e||v!=expected)printf("reg%d=%llx expected=%llx r=%x\n",r,v,expected,e);return !e&&v==expected;}
int main(){
    setvbuf(stdout,nullptr,_IONBF,0);
    if(hv_vm_create(HV_VM_DEFAULT)){check(false,"create event VM");return 1;}
    hv_vcpuid_t c;if(hv_vcpu_create(&c,HV_VCPU_DEFAULT)){hv_vm_destroy();return 1;}
    void *mem=nullptr;if(posix_memalign(&mem,4096,8*4096)){hv_vcpu_destroy(c);hv_vm_destroy();return 1;}
    memset(mem,0,8*4096);auto b=static_cast<unsigned char *>(mem);auto code=b+4*4096;auto t=static_cast<uint64_t *>(mem);
    t[0]=0x2003;t[512]=0x3003;t[1024]=0x4003;for(unsigned i=8;i<12;++i)t[1536+i]=(i*4096)|3;
    bool ok=hv_vm_map(mem,0x1000,4*4096,HV_MEMORY_READ|HV_MEMORY_WRITE)==0 &&
        hv_vm_map(code,0x8000,4*4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC)==0;
    ok &= hv_vmx_vcpu_write_vmcs(c,0x6800,0x80010033)==0;
    ok &= hv_vmx_vcpu_write_vmcs(c,0x6802,0x1000)==0;
    ok &= hv_vmx_vcpu_write_vmcs(c,0x6804,0x20)==0;
    ok &= hv_vmx_vcpu_write_vmcs(c,0x2806,0xd00)==0;
    for(unsigned i=0;i<6;++i){ok &= hv_vmx_vcpu_write_vmcs(c,0x800+i*2,i==1?8:16)==0;
        ok &= hv_vmx_vcpu_write_vmcs(c,0x4800+i*2,0xffffffff)==0;
        ok &= hv_vmx_vcpu_write_vmcs(c,0x4814+i*2,i==1?0xa09b:0xc093)==0;}
    uint64_t gdt[]={0,0x00af9b000000ffffull,0x00cf93000000ffffull};memcpy(code+0x2000,gdt,sizeof(gdt));
    hv_vmx_vcpu_write_vmcs(c,0x6816,0xa000);hv_vmx_vcpu_write_vmcs(c,0x4810,23);
    hv_vmx_vcpu_write_vmcs(c,0x6818,0xb000);hv_vmx_vcpu_write_vmcs(c,0x4812,4095);
    auto gate=code+0x3400;gate[0]=0;gate[1]=0x81;gate[2]=8;gate[5]=0x8e;
    code[0]=0xf4;
    const unsigned char handler[]={0x48,0x8b,0x14,0x24,0x48,0x8b,0x4c,0x24,0x10,
        0x4c,0x8b,0x44,0x24,0x18,0x4c,0x8b,0x4c,0x24,0x20,0x9c,0x58,0x48,0xcf};
    memcpy(code+0x100,handler,sizeof(handler));check(ok,"configure real guest GDT/IDT/stack and IRQ handler");
    auto entry=[&](uint64_t rsp){hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vcpu_write_register(c,HV_X86_RSP,rsp);
        hv_vcpu_write_register(c,HV_X86_RFLAGS,0x246);hv_vmx_vcpu_write_vmcs(c,0x4824,0);hv_vmx_vcpu_write_vmcs(c,0x4016,0x80000040);};
    auto run=[&]() -> hv_return_t {for(unsigned n=0;n<64;++n){auto r=hv_vcpu_run(c);if(r)return r;uint64_t reason=0;
        hv_vmx_vcpu_read_vmcs(c,VMCS_RO_EXIT_REASON,&reason);if(reason!=1)return HV_SUCCESS;}return HV_ERROR;};
    // A finite no-intercept workload validates the timer exit path before any
    // infinite guest is tried. A broken timer path still reaches HLT eventually.
    const unsigned char finite[]={0xb9,0x00,0xc2,0xeb,0x0b,0xff,0xc9,0x75,0xfc,0xf4};
    memcpy(code,finite,sizeof(finite));hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
    hv_vcpu_write_register(c,HV_X86_RFLAGS,2);unsigned hostExits=0;double worst=0;bool halted=false;
    for(unsigned i=0;i<10000;++i){auto begin=std::chrono::steady_clock::now();auto r=hv_vcpu_run(c);
        double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();if(ms>worst)worst=ms;
        if(r)break;uint64_t reason=0;hv_vmx_vcpu_read_vmcs(c,VMCS_RO_EXIT_REASON,&reason);
        if(reason==12){halted=true;break;}if(reason!=1)break;++hostExits;
    }
    printf("fast finite workload: host exits=%u worst call=%.3fms\n",hostExits,worst);
    check(halted && hostExits>0 && worst<500,"finite IF0 workload is preempted by physical host IRQs without TF");
    if(halted && hostExits>0 && worst<500){
        code[0]=0xeb;code[1]=0xfe;
        for(uint64_t flags : {2ull,0x202ull}){
            hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vcpu_write_register(c,HV_X86_RFLAGS,flags);
            bool bounded=true;for(unsigned n=0;n<10;++n){auto begin=std::chrono::steady_clock::now();
                bounded &= hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,1);
                bounded &= std::chrono::steady_clock::now()-begin<std::chrono::milliseconds(500);}
            check(bounded,"infinite JMP guest yields to host timer with guest IF0/IF1");
        }
        hv_vcpu_interrupt(&c,1);check(hv_vcpu_run(c)==0&&field(c,VMCS_RO_EXIT_REASON,1),"cancellation mailbox remains usable in fast mode");
    }else check(false,"infinite guest skipped because timer preemption was not established");
    const unsigned char idToggle[]={0x9c,0x58,0x35,0,0,0x20,0,0x50,0x9d,0x0f,0xa2};
    memcpy(code,idToggle,sizeof(idToggle));hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
    hv_vcpu_write_register(c,HV_X86_RSP,0x9080);hv_vcpu_write_register(c,HV_X86_RFLAGS,0x202);
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,10)&&reg(c,HV_X86_RFLAGS,0x200202)&&reg(c,HV_X86_RSP,0x9080),
          "native fast PUSHF/POPF CPUID discovery retains ID across the CPUID exit");
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,10)&&reg(c,HV_X86_RFLAGS,0x200202),"fast re-entry preserves ID without rewriting flags");
    hv_vcpu_write_register(c,HV_X86_RFLAGS,2);
    const unsigned char de[]={0x0f,0x20,0xe0,0x83,0xc8,8,0x0f,0x22,0xe0,0x0f,0x20,0xe1,0xf4};
    memcpy(code,de,sizeof(de));hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vcpu_write_register(c,HV_X86_RFLAGS,2);
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,12)&&reg(c,HV_X86_RCX,0x28)&&field(c,0x6804,0x28),
          "CR4.DE roundtrip survives fast-mode re-entry");
    // Keep DE enabled through the remaining timer/event/TPR tests.
    const unsigned char cr2write[]={0x0f,0x22,0xd0,0xf4};
    memcpy(code,cr2write,sizeof(cr2write));hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
    hv_vcpu_write_register(c,HV_X86_RAX,0xfedcba9876543210ull);
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,12)&&reg(c,HV_X86_CR2,0xfedcba9876543210ull),
          "fast MOV CR2 publishes full guest value");
    const unsigned char cr2read[]={0x0f,0x20,0xd1,0xf4};
    memcpy(code,cr2read,sizeof(cr2read));hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
    check(run()==0&&reg(c,HV_X86_RCX,0xfedcba9876543210ull),"fast CR2 survives separate entries");
    // CR8 maps to the framework's byte-sized APIC TPR, including VMX control exits.
    const unsigned char cr8[]={0xb8,9,0,0,0,0x44,0x0f,0x22,0xc0,0x44,0x0f,0x20,0xc1,0xf4};
    memcpy(code,cr8,sizeof(cr8));hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vcpu_write_register(c,HV_X86_RFLAGS,2);
    check(run()==0&&reg(c,HV_X86_RCX,9)&&reg(c,HV_X86_TPR,0x90),"CR8 write/read roundtrip publishes TPR byte");
    code[0]=0x44;code[1]=0x0f;code[2]=0x20;code[3]=0xc0;code[4]=0xf4;
    hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vmx_vcpu_write_vmcs(c,0x4002,0x100000);
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,28)&&field(c,VMCS_RO_EXIT_QUALIFIC,0x18)&&reg(c,HV_X86_RIP,0x8000),"CR8 load-exiting returns VMX28 before consuming instruction");
    hv_vmx_vcpu_write_vmcs(c,0x4002,0);hv_vcpu_write_register(c,HV_X86_TPR,0);
    code[0]=0xfa;code[1]=0xfb;code[2]=0x90;code[3]=0xf4;
    hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vcpu_write_register(c,HV_X86_RFLAGS,2);hv_vmx_vcpu_write_vmcs(c,0x4002,4);
    check(run()==0&&field(c,VMCS_RO_EXIT_REASON,7)&&reg(c,HV_X86_RIP,0x8003),"hardware VINTR window opens after STI shadow with no debug TF");
    hv_vmx_vcpu_write_vmcs(c,0x4002,0);code[0]=0xf4;
    for(bool trap : {false,true}){
        gate[5]=trap?0x8f:0x8e;entry(0x9087);
        check(run()==0 && field(c,VMCS_RO_EXIT_REASON,12) && reg(c,HV_X86_RIP,0x8000),"IRQ handler executes and IRETQ returns to interrupted HLT");
        check(reg(c,HV_X86_RDX,0x8000)&&reg(c,HV_X86_RCX,0x246)&&reg(c,HV_X86_R8,0x9087)&&reg(c,HV_X86_R9,16),
              "saved IRQ frame has RIP/RFLAGS/RSP/SS without internal TF");
        check(reg(c,HV_X86_RAX,trap?0x246:0x46),"handler PUSHF sees interrupt/trap-gate IF semantics without TF");
        check(reg(c,HV_X86_RSP,0x9087)&&reg(c,HV_X86_RFLAGS,0x246)&&field(c,0x4016,0),"IRETQ restores unaligned original stack/flags and event is consumed once");
    }
    check(hv_vm_unmap(0x1000,4*4096)==0&&hv_vm_unmap(0x8000,4*4096)==0,"unmap fast guest");
    free(mem);check(hv_vcpu_destroy(c)==0&&hv_vm_destroy()==0,"fast cleanup");return failures?1:0;
}
