#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <sys/sysctl.h>
static unsigned failures;
static void check(bool ok,const char *label){printf("%s: %s\n",ok?"PASS":"FAIL",label);failures+=!ok;}
static bool field(hv_vcpuid_t c,unsigned f,uint64_t expected){uint64_t v=~0ull;auto r=hv_vmx_vcpu_read_vmcs(c,f,&v);if(r||v!=expected)printf("VMCS%x=%llx expected=%llx r=%x\n",f,v,expected,r);return !r&&v==expected;}
static bool reg(hv_vcpuid_t c,hv_x86_reg_t r,uint64_t expected){uint64_t v=~0ull;auto e=hv_vcpu_read_register(c,r,&v);if(e||v!=expected)printf("reg%d=%llx expected=%llx r=%x\n",r,v,expected,e);return !e&&v==expected;}
int main(int argc,char **argv){
    bool high=false,hardware=false;
    for(int i=1;i<argc;++i){
        if(!strcmp(argv[i],"--high"))high=true;
        else if(!strcmp(argv[i],"--hardware"))hardware=true;
        else return 2;
    }
    int fast=0;size_t fastSize=sizeof(fast);
    if(sysctlbyname("debug.amdhv.fast",&fast,&fastSize,nullptr,0)||bool(fast)!=hardware)return 2;
    const uint64_t va=high?0xffff800000000000ull:0;
    printf("EVENT high=%d hardware=%d\n",high,hardware);
    setvbuf(stdout,nullptr,_IONBF,0);
    if(hv_vm_create(HV_VM_DEFAULT)){check(false,"create event VM");return 1;}
    hv_vcpuid_t c;if(hv_vcpu_create(&c,HV_VCPU_DEFAULT)){hv_vm_destroy();return 1;}
    void *mem=nullptr;if(posix_memalign(&mem,4096,8*4096)){hv_vcpu_destroy(c);hv_vm_destroy();return 1;}
    memset(mem,0,8*4096);auto b=static_cast<unsigned char *>(mem);auto code=b+4*4096;auto t=static_cast<uint64_t *>(mem);
    t[0]=0x2003;t[256]=0x2003;t[512]=0x3003;t[1024]=0x4003;for(unsigned i=8;i<12;++i)t[1536+i]=(i*4096)|3;
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
    hv_vmx_vcpu_write_vmcs(c,0x6816,va+0xa000);hv_vmx_vcpu_write_vmcs(c,0x4810,23);
    hv_vmx_vcpu_write_vmcs(c,0x6818,va+0xb000);hv_vmx_vcpu_write_vmcs(c,0x4812,4095);
    auto gate=code+0x3400;gate[0]=0;gate[1]=0x81;gate[2]=8;gate[5]=0x8e;
    const uint64_t handlerVa=va+0x8100;
    gate[6]=uint8_t(handlerVa>>16);gate[7]=uint8_t(handlerVa>>24);
    for(unsigned i=0;i<4;++i)gate[8+i]=uint8_t(handlerVa>>(32+i*8));
    code[0]=0xf4;
    const unsigned char handler[]={0x48,0x8b,0x14,0x24,0x48,0x8b,0x4c,0x24,0x10,
        0x4c,0x8b,0x44,0x24,0x18,0x4c,0x8b,0x4c,0x24,0x20,0x9c,0x58,0x48,0xcf};
    memcpy(code+0x100,handler,sizeof(handler));check(ok,"configure real guest GDT/IDT/stack and IRQ handler");
    auto entry=[&](uint64_t rsp){hv_vcpu_write_register(c,HV_X86_RIP,va+0x8000);hv_vcpu_write_register(c,HV_X86_RSP,rsp);
        hv_vcpu_write_register(c,HV_X86_RFLAGS,0x246);hv_vmx_vcpu_write_vmcs(c,0x4824,0);hv_vmx_vcpu_write_vmcs(c,0x4016,0x80000040);};
    auto run=[&]() -> hv_return_t {for(unsigned n=0;n<64;++n){auto r=hv_vcpu_run(c);if(r)return r;uint64_t reason=0;
        hv_vmx_vcpu_read_vmcs(c,VMCS_RO_EXIT_REASON,&reason);if(reason!=1)return HV_SUCCESS;}return HV_ERROR;};
    if(high){
        const unsigned char stateCode[]={0xf3,0x90,0x9c,0x58,0x50,0x9d,0x0f,0x21,0xc1,0x0f,0xa2};
        memcpy(code,stateCode,sizeof(stateCode));
        hv_vcpu_write_register(c,HV_X86_RIP,va+0x8000);hv_vcpu_write_register(c,HV_X86_RSP,va+0x9087);
        hv_vcpu_write_register(c,HV_X86_RFLAGS,0x246);
        check(run()==0&&field(c,VMCS_RO_EXIT_REASON,10)&&reg(c,HV_X86_RAX,0x246)&&reg(c,HV_X86_RSP,va+0x9087),
              "high code/stack PAUSE, PUSHF/POPF and intercepted MOV DR preserve flags and stack");
        hv_vcpu_write_register(c,HV_X86_RIP,0x800000000000ull);
        check(hv_vcpu_run(c)==HV_UNSUPPORTED,"noncanonical RIP is rejected before guest entry");
        if(!hardware){
            hv_vcpu_write_register(c,HV_X86_RIP,va+0x8000);hv_vmx_vcpu_write_vmcs(c,0x2806,0xd01);
            check(hv_vcpu_run(c)==HV_UNSUPPORTED&&reg(c,HV_X86_RIP,va+0x8000),
                  "step runner rejects SCE before SYSCALL could clear its internal TF");
            hv_vmx_vcpu_write_vmcs(c,0x2806,0xd00);
        }
        code[0]=0xf4;
    }
    for(bool trap : {false,true}){
        gate[5]=trap?0x8f:0x8e;entry(va+0x9087);
        check(run()==0 && field(c,VMCS_RO_EXIT_REASON,12) && reg(c,HV_X86_RIP,va+0x8000),"IRQ handler executes and IRETQ returns to interrupted HLT");
        check(reg(c,HV_X86_RDX,va+0x8000)&&reg(c,HV_X86_RCX,0x246)&&reg(c,HV_X86_R8,va+0x9087)&&reg(c,HV_X86_R9,16),
              "saved IRQ frame has RIP/RFLAGS/RSP/SS without internal TF");
        check(reg(c,HV_X86_RAX,trap?0x246:0x46),"handler PUSHF sees interrupt/trap-gate IF semantics without TF");
        check(reg(c,HV_X86_RSP,va+0x9087)&&reg(c,HV_X86_RFLAGS,0x246)&&field(c,0x4016,0),"IRETQ restores unaligned original stack/flags and event is consumed once");
    }
    if(!hardware){
    // Failed cross-page preflight must not leave an initial frame fragment behind.
    gate[5]=0x8e;memset(code+0xfe8,0x5a,40);unsigned char saved[40];memcpy(saved,code+0xfe8,40);
    hv_vm_protect(0x9000,4096,HV_MEMORY_READ);entry(va+0x9017);
    check(hv_vcpu_run(c)==HV_UNSUPPORTED&&reg(c,HV_X86_RIP,va+0x8000)&&reg(c,HV_X86_RSP,va+0x9017)&&!memcmp(saved,code+0xfe8,40),
          "read-only second stack page rejects IRQ before writing either frame fragment");
    hv_vm_protect(0x9000,4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC);
    gate[4]=1;entry(va+0x9087);
    check(hv_vcpu_run(c)==HV_UNSUPPORTED&&reg(c,HV_X86_RIP,va+0x8000),"unsupported IST entry is rejected before guest execution");gate[4]=0;
    entry(va+0x9087);hv_vmx_vcpu_write_vmcs(c,0x4016,0x80000202);
    check(hv_vcpu_run(c)==HV_UNSUPPORTED,"NMI injection remains unsupported");hv_vmx_vcpu_write_vmcs(c,0x4016,0);
    code[0x180]=0x48;code[0x181]=0xcf;uint64_t badFrame[]={va+0x8000,8,0x1246,va+0x9087,16};memcpy(code+0x1080,badFrame,sizeof(badFrame));
    hv_vcpu_write_register(c,HV_X86_RIP,va+0x8180);hv_vcpu_write_register(c,HV_X86_RSP,va+0x9080);hv_vcpu_write_register(c,HV_X86_RFLAGS,2);
    check(hv_vcpu_run(c)==HV_UNSUPPORTED&&reg(c,HV_X86_RIP,va+0x8180)&&reg(c,HV_X86_RSP,va+0x9080),"IRETQ unsupported flags reject before committing return state");
    }
    if(high&&hardware){
        bool syscallOK=hv_vmx_vcpu_write_vmcs(c,0x2806,0xd01)==0;
        for(uint32_t msr : {0xc0000081u,0xc0000082u,0xc0000084u})
            syscallOK &= hv_vcpu_enable_managed_msr(c,msr,true)==0;
        syscallOK &= hv_vcpu_write_msr(c,0xc0000081,8ull<<32)==0;
        syscallOK &= hv_vcpu_write_msr(c,0xc0000082,va+0x8180)==0;
        syscallOK &= hv_vcpu_write_msr(c,0xc0000084,0x300)==0;
        code[0]=0x0f;code[1]=0x05;code[0x180]=0x9c;code[0x181]=0x58;code[0x182]=0xf4;
        hv_vcpu_write_register(c,HV_X86_RIP,va+0x8000);hv_vcpu_write_register(c,HV_X86_RSP,va+0x9087);
        hv_vcpu_write_register(c,HV_X86_RFLAGS,0x246);hv_vmx_vcpu_write_vmcs(c,0x4016,0);
        check(syscallOK&&run()==0&&field(c,VMCS_RO_EXIT_REASON,12)&&reg(c,HV_X86_RIP,va+0x8182)&&
              reg(c,HV_X86_RCX,va+0x8002)&&reg(c,HV_X86_R11,0x246)&&reg(c,HV_X86_RAX,0x46)&&reg(c,HV_X86_RSP,va+0x9087),
              "fast SCE/SYSCALL uses guest STAR/LSTAR/SFMASK and high guest code, without a host syscall");
    }
    if(high){
        t[1536+12]=0xc003;
        hv_vcpu_write_register(c,HV_X86_RIP,va+0xc000);hv_vcpu_write_register(c,HV_X86_RSP,va+0x9087);
        hv_vcpu_write_register(c,HV_X86_RFLAGS,2);hv_vmx_vcpu_write_vmcs(c,0x4016,0);
        check(run()==0&&field(c,VMCS_RO_EXIT_REASON,48)&&field(c,VMCS_GUEST_PHYSICAL_ADDRESS,0xc000),
              "high virtual code still obeys NPT ownership of low guest physical memory");
    }
    check(hv_vm_unmap(0x1000,4*4096)==0&&hv_vm_unmap(0x8000,4*4096)==0,"unmap IRQ guest");
    free(mem);check(hv_vcpu_destroy(c)==0&&hv_vm_destroy()==0,"IRQ cleanup");return failures?1:0;
}
