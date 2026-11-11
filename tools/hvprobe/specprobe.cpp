#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <sys/sysctl.h>
#include <cpuid.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "../../AMDHV/src/speculation_policy.hpp"
static unsigned failures;
static void check(bool ok,const char *s){printf("%s: %s\n",ok?"PASS":"FAIL",s);failures+=!ok;}
static bool field(hv_vcpuid_t c,unsigned f,uint64_t expected){uint64_t v=~0ull;auto r=hv_vmx_vcpu_read_vmcs(c,f,&v);
    if(r||v!=expected)printf("VMCS%x=%llx expected=%llx r=%x\n",f,v,expected,r);return !r&&v==expected;}
static bool reg(hv_vcpuid_t c,hv_x86_reg_t r,uint64_t expected){uint64_t v=~0ull;auto e=hv_vcpu_read_register(c,r,&v);
    if(e||v!=expected)printf("reg%d=%llx expected=%llx r=%x\n",r,v,expected,e);return !e&&v==expected;}
int main(){
    setvbuf(stdout,nullptr,_IONBF,0);int fast=0;size_t n=sizeof(fast);
    if(sysctlbyname("debug.amdhv.fast",&fast,&n,nullptr,0)||fast!=1)return 2;
    unsigned a,b,cx,d;__cpuid_count(0x80000008,0,a,b,cx,d);unsigned ebx8=b;
    __cpuid_count(0x8000000a,0,a,b,cx,d);unsigned edxA=d;
    __cpuid_count(0x80000000,0,a,b,cx,d);unsigned eax21=0;
    if(a>=0x80000021){__cpuid_count(0x80000021,0,a,b,cx,d);eax21=a;}
    __cpuid_count(7,0,a,b,cx,d);
    const auto caps=amdhv::speculationCaps(ebx8,edxA,eax21,d);
    printf("physical SPEC_CTRL mask=%llx PRED_CMD mask=%llx virtual=%u L1D=%u\n",caps.controls,caps.commands,caps.virtualControls,caps.l1dFlush);
    if(!caps.virtualControls||!caps.controls||!(caps.commands&1)){puts("Missing required physical speculation capabilities");return 2;}
    if(hv_vm_create(HV_VM_DEFAULT))return 2;
    hv_vcpuid_t c;if(hv_vcpu_create(&c,HV_VCPU_DEFAULT)){hv_vm_destroy();return 2;}
    void *mem=nullptr;if(posix_memalign(&mem,4096,9*4096)){hv_vcpu_destroy(c);hv_vm_destroy();return 2;}
    memset(mem,0,9*4096);auto code=static_cast<unsigned char*>(mem)+4*4096;auto t=static_cast<uint64_t*>(mem);
    constexpr uint64_t va=0xffff800000000000ull;
    t[0]=t[256]=0x2003;t[512]=0x3003;t[1024]=0x4003;
    for(unsigned i=8;i<13;++i)t[1536+i]=i*4096+3;
    bool ok=!hv_vm_map(mem,0x1000,4*4096,HV_MEMORY_READ|HV_MEMORY_WRITE)&&
        !hv_vm_map(code,0x8000,5*4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC);
    auto configure=[&](){bool good=true;
        const struct{unsigned f;uint64_t v;} fields[]={{0x6800,0x80010033},{0x6802,0x1000},{0x6804,0x20},{0x2806,0xd00},
            {0x6816,va+0xa000},{0x4810,23},{0x6818,va+0xb000},{0x4812,4095}};
        for(auto f:fields)good &= !hv_vmx_vcpu_write_vmcs(c,f.f,f.v);
        for(unsigned i=0;i<6;++i){good &= !hv_vmx_vcpu_write_vmcs(c,0x800+i*2,i==1?8:16);
            good &= !hv_vmx_vcpu_write_vmcs(c,0x4800+i*2,0xffffffff);
            good &= !hv_vmx_vcpu_write_vmcs(c,0x4814+i*2,i==1?0xa09b:0xc093);}
        return good;};
    uint64_t gdt[]={0,0x00af9b000000ffffull,0x00cf93000000ffffull};memcpy(code+0x2000,gdt,sizeof(gdt));
    auto gate=code+0x3000+13*16;uint64_t handler=va+0x8100;
    gate[0]=handler;gate[1]=handler>>8;gate[2]=8;gate[5]=0x8e;gate[6]=handler>>16;gate[7]=handler>>24;
    for(unsigned i=0;i<4;++i)gate[8+i]=handler>>(32+i*8);
    const unsigned char gp[]={0x4c,0x8b,0x04,0x24,0x4c,0x8b,0x4c,0x24,8,
        0x48,0x83,0x44,0x24,8,2,0x48,0x83,0xc4,8,0x48,0xcf};
    memcpy(code+0x100,gp,sizeof(gp));check(ok&&configure(),"configure native speculation MSR guest and real GP handler");
    auto prepare=[&](uint32_t address,uint64_t value,bool write,bool prefix=false){
        unsigned i=0;if(prefix)code[i++]=0x66;code[i++]=0x0f;code[i++]=write?0x30:0x32;code[i++]=0x0f;code[i++]=0xa2;
        hv_vcpu_write_register(c,HV_X86_RCX,address);hv_vcpu_write_register(c,HV_X86_RAX,uint32_t(value));
        hv_vcpu_write_register(c,HV_X86_RDX,value>>32);hv_vcpu_write_register(c,HV_X86_RIP,va+0x8000);
        hv_vcpu_write_register(c,HV_X86_RSP,va+0x9087);hv_vcpu_write_register(c,HV_X86_RFLAGS,0x246);
        hv_vcpu_write_register(c,HV_X86_R8,~0ull);hv_vcpu_write_register(c,HV_X86_R9,~0ull);
        hv_vmx_vcpu_write_vmcs(c,0x4824,0);hv_vmx_vcpu_write_vmcs(c,0x4016,0);};
    auto run=[&](){for(unsigned i=0;i<256;++i){auto r=hv_vcpu_run(c);if(r)return false;
        uint64_t reason=0;hv_vmx_vcpu_read_vmcs(c,0x4402,&reason);if(reason!=1)return reason==10;}return false;};
    auto read=[&](uint64_t expected){prepare(0x48,0,false);return run()&&reg(c,HV_X86_RAX,uint32_t(expected))&&reg(c,HV_X86_RDX,expected>>32);};
    check(read(0),"new vCPU starts with guest SPEC_CTRL zero rather than the host value");
    for(uint64_t value:{0ull,1ull,2ull,4ull,0x80ull,caps.controls})if(!(value&~caps.controls)){
        prepare(0x48,value,true);check(run()&&reg(c,HV_X86_RIP,va+0x8002)&&read(value),"SPEC_CTRL accepted bits persist across VMRUN and API re-entry; host value checked by kernel");}
    prepare(0x48,caps.controls,true,true);
    check(run()&&reg(c,HV_X86_RIP,va+0x8003)&&read(caps.controls),"prefixed SPEC_CTRL uses hardware NRIP, not a fixed two-byte advance");
    bool retained=true;for(unsigned i=0;i<32;++i)retained &= read(caps.controls);
    check(retained,"guest speculation state survives repeated host exits and re-entry");
    for(uint64_t value:{0ull,1ull,0x80ull,0x81ull})if(!(value&~caps.commands)){
        prepare(0x49,value,true);check(run()&&reg(c,HV_X86_RIP,va+0x8002)&&read(caps.controls),"supported PRED_CMD barrier completes without replacing guest SPEC_CTRL");}
    prepare(0x49,1,true,true);check(run()&&reg(c,HV_X86_RIP,va+0x8003),"prefixed IBPB request advances through the complete instruction");
    if(caps.l1dFlush)for(uint64_t value:{0ull,1ull}) {
        prepare(0x10b,value,true);check(run()&&reg(c,HV_X86_RIP,va+0x8002)&&read(caps.controls),"physical L1D flush command completes without replacing guest controls");
    }
    auto fault=[&](uint32_t address,uint64_t value,bool write,const char *label){prepare(address,value,write);
        check(run()&&reg(c,HV_X86_R8,0)&&reg(c,HV_X86_R9,va+0x8000)&&reg(c,HV_X86_RIP,va+0x8002)&&read(caps.controls),label);};
    fault(0x49,0,false,"write-only PRED_CMD read delivers native GP(0) and returns through IRET");
    fault(0x49,2,true,"reserved PRED_CMD bit raises GP before any physical command");
    fault(0x49,1ull<<32,true,"upper EDX PRED_CMD bits are validated, not truncated");
    fault(0x48,8,true,"reserved SPEC_CTRL bit raises GP without changing the guest control");
    fault(0x10b,0,false,"write-only FLUSH_CMD read raises GP");
    fault(0x10b,2,true,"reserved L1D flush command bits raise GP before physical execution");
    hv_vmx_vcpu_write_vmcs(c,0x4004,1u<<13);prepare(0x49,0,false);
    bool direct=false;for(unsigned i=0;i<256;++i){auto r=hv_vcpu_run(c);if(r)break;uint64_t reason=0;hv_vmx_vcpu_read_vmcs(c,0x4402,&reason);
        if(reason!=1){direct=reason==0;break;}}
    check(direct&&field(c,0x4404,0x80000b0d)&&field(c,0x4406,0)&&reg(c,HV_X86_RIP,va+0x8000),"invalid speculation access honors the GP bitmap without advancing RIP");
    hv_vmx_vcpu_write_vmcs(c,0x4004,0);
    check(!hv_vcpu_destroy(c),"destroy vCPU holding a nonzero guest speculation state");
    check(!hv_vcpu_create(&c,HV_VCPU_DEFAULT)&&configure()&&read(0),"reused vCPU slot cannot inherit prior guest SPEC_CTRL");
    check(!hv_vcpu_destroy(c),"destroy reset speculation vCPU");
    check(!hv_vm_unmap(0x1000,4*4096)&&!hv_vm_unmap(0x8000,5*4096),"unmap speculation guest");
    free(mem);check(!hv_vm_destroy(),"speculation guest cleanup");return failures?1:0;
}
