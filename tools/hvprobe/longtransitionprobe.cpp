#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <sys/sysctl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "segment_diagnostic.hpp"
static unsigned failures;
static void check(bool ok,const char *name) { printf("%s: %s\n",ok?"PASS":"FAIL",name); failures+=!ok; }
static bool field(hv_vcpuid_t c,unsigned f,uint64_t expected) {
    uint64_t v=~0ull; auto r=hv_vmx_vcpu_read_vmcs(c,f,&v);
    if(r || v!=expected) printf("field%x=%llx expected=%llx result=%x\n",f,v,expected,r);
    return r==0 && v==expected;
}
int main(int argc,char **argv) {
    const bool diagnostic=argc==2&&!strcmp(argv[1],"--diagnostic-normalize-segments");
    const bool sce=argc==2&&!strcmp(argv[1],"--sce");
    if(argc!=1&&!diagnostic&&!sce)return 2;
    setvbuf(stdout,nullptr,_IONBF,0);
    if(sce){int fast=0;size_t size=sizeof(fast);
        if(sysctlbyname("debug.amdhv.fast",&fast,&size,nullptr,0)||fast!=1){puts("SCE transition requires fast mode");return 2;}
        puts("SCE AP transition: EFER901 before PG, then d01 through compatibility entry and far jump64");}
    if(diagnostic)puts("DIAGNOSTIC ONLY: userspace segment normalization enabled; does not validate unmodified kext");
    if(hv_vm_create(HV_VM_DEFAULT)) return 2;
    hv_vcpuid_t c; if(hv_vcpu_create(&c,HV_VCPU_DEFAULT)) { hv_vm_destroy(); return 2; }
    void *p=nullptr; if(posix_memalign(&p,4096,5*4096)) return 2;
    memset(p,0,5*4096); auto b=static_cast<unsigned char*>(p);
    auto t=static_cast<uint64_t*>(p);
    t[0]=0x2003; t[512]=0x3003; t[1024]=0x4003; t[1536+8]=0x8003;
    const unsigned char code32[]={0x31,0xc0,0x0f,0x00,0xd0, // LLDT null
        0xb8,0x20,0,0,0,0x0f,0x00,0xd8, // LTR guest TSS before paging
        0x0f,0x20,0xc0,0x0f,0xba,0xe8,0x1f,0x0f,0x22,0xc0,
        0xea,0x40,0x80,0,0,0x18,0}; // CR0.PG write, then far jump to long CS
    const unsigned char code64[]={0x48,0xb8,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,
        0x48,0xbb,0,0x82,0,0,0,0,0,0,0x48,0x89,0x03,0x0f,0xa2,0xf4};
    memcpy(b+4*4096,code32,sizeof(code32)); memcpy(b+4*4096+0x40,code64,sizeof(code64));
    uint64_t gdt[]={0,0x00cf93000000ffffull,0x00cf9b000000ffffull,0x00af9b000000ffffull,
        0x0000890084000067ull,0};
    memcpy(b+4*4096+0x180,gdt,sizeof(gdt));
    bool ok=hv_vm_map(p,0x1000,4*4096,HV_MEMORY_READ|HV_MEMORY_WRITE)==0 &&
        hv_vm_map(b+4*4096,0x8000,4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC)==0;
    const struct {unsigned f; uint64_t v;} state[]={
        {0x6800,0x23},{0x6802,0x1000},{0x6804,0x2660},{0x2806,0x100},
        {0x6000,0xe0000020},{0x6004,0x40000023},{0x6002,0x2000},
        {0x4002,0x200080},{0x6816,0x8180},{0x4810,47},{0x681e,0x8000},{0x681c,0x8ff0}};
    for(auto e:state) ok &= hv_vmx_vcpu_write_vmcs(c,e.f,e.v)==0;
    if(sce)ok &= hv_vmx_vcpu_write_vmcs(c,0x2806,0x901)==0;
    for(unsigned i=0;i<6;++i) {
        ok &= hv_vmx_vcpu_write_vmcs(c,0x800+i*2,i==1?0x10:8)==0;
        ok &= hv_vmx_vcpu_write_vmcs(c,0x4800+i*2,0xffffffff)==0;
        ok &= hv_vmx_vcpu_write_vmcs(c,0x4814+i*2,i==1?0xc09b:0xc093)==0;
    }
    check(ok,"configure native LME preparation state with PAE and guest page tables");
    auto run=[&]() -> hv_return_t {auto result=hv_vcpu_run(c);
        if(diagnostic&&!normalizeDiagnosticSegments(c))return HV_ERROR;return result;};
    auto r=run();
    check(r==0 && field(c,VMCS_RO_EXIT_REASON,28) && field(c,VMCS_RO_EXIT_QUALIFIC,0) &&
        field(c,VMCS_GUEST_RIP,0x8014),"LME preparation runs; masked PG write exits before execution");
    if(sce)check(field(c,0x2806,0x901),"SCE/NXE survive the prepared-LME exit without synthesizing LMA");
    check(field(c,0x80e,0x20)&&field(c,0x6814,0x8400)&&field(c,0x4822,0x8b)&&field(c,0x4820,0x10000),
          "Linux-style LLDT/LTR state is published before the paging transition");
    uint64_t ax=0;
    check(hv_vcpu_read_register(c,HV_X86_RAX,&ax)==0 && ax==0xc0000033,"PG write operand preserves virtual CR0 and fixed ET");
    // Model userspace VMM's handling of masked PG, as observed in native startup.
    ok=hv_vmx_vcpu_write_vmcs(c,VMCS_GUEST_CR0,0x80000023)==0 &&
        hv_vmx_vcpu_write_vmcs(c,VMCS_GUEST_IA32_EFER,sce?0xd01:0x500)==0 &&
        hv_vmx_vcpu_write_vmcs(c,VMCS_CTRL_CR0_SHADOW,0xc0000023)==0 &&
        hv_vmx_vcpu_write_vmcs(c,VMCS_CTRL_VMENTRY_CONTROLS,0x200)==0 &&
        hv_vcpu_write_register(c,HV_X86_RIP,0x8017)==0;
    check(ok,"publish PG/LMA and VMX IA-32e entry state");
    r=run();
    check(r==0 && field(c,VMCS_RO_EXIT_REASON,10) && field(c,VMCS_GUEST_CS,0x18),"compatibility entry far-jumps to 64-bit code and reaches CPUID");
    if(sce)check(field(c,0x2806,0xd01),"SCE/NXE/LMA remain intact after native compatibility-to-code64 transition");
    uint64_t marker=0; memcpy(&marker,b+4*4096+0x200,8);
    check(marker==0x1122334455667788ull,"64-bit guest stores full-width marker through guest paging and NPT");
    check(field(c,0x80e,0x20)&&field(c,0x6814,0x8400)&&field(c,0x4820,0x10000),
          "TSS and null LDTR caches survive compatibility-to-long-mode transition");
    hv_vcpu_write_register(c,HV_X86_RIP,0x8059);
    check(run()==0 && field(c,VMCS_RO_EXIT_REASON,12),"resume native-layout long mode reaches HLT");
    check(hv_vm_unmap(0x4000,4096)==0,"remove guest leaf table backing");
    hv_vcpu_write_register(c,HV_X86_RIP,0x8040);
    check(run()==0 && field(c,VMCS_RO_EXIT_REASON,48) && field(c,VMCS_GUEST_PHYSICAL_ADDRESS,0x4040),"long transition retains NPT protection for page walks");
    check(hv_vm_unmap(0x1000,3*4096)==0 && hv_vm_unmap(0x8000,4096)==0,"unmap transition guest");
    free(p); check(hv_vcpu_destroy(c)==0 && hv_vm_destroy()==0,"transition guest cleanup");
    return failures?1:0;
}
