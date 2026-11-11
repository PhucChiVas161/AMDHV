#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static unsigned failures;
static void check(bool ok,const char *label) { printf("%s: %s\n",ok?"PASS":"FAIL",label); failures+=!ok; }
static bool field(hv_vcpuid_t c,unsigned f,uint64_t expected) {
    uint64_t v=~0ull; return hv_vmx_vcpu_read_vmcs(c,f,&v)==0 && v==expected;
}
int main() {
    setvbuf(stdout,nullptr,_IONBF,0);
    if(hv_vm_create(HV_VM_DEFAULT)) return 2;
    hv_vcpuid_t c; if(hv_vcpu_create(&c,HV_VCPU_DEFAULT)) {hv_vm_destroy(); return 2;}
    void *p=nullptr; if(posix_memalign(&p,4096,4096)) return 2;
    memset(p,0,4096);
    const unsigned char code[]={0x0f,0x20,0xc0,0x89,0xc3, // CR0 -> EBX
        0xb8,0x40,6,0,0,0x0f,0x22,0xe0, // CR4=640
        0xb8,0,0,0x80,0,0x0f,0x22,0xd8,0x0f,0x20,0xda, // CR3=800000 -> EDX
        0xb8,0x60,6,0,0,0x0f,0x22,0xe0,0x0f,0x20,0xe1,0x0f,0xa2,0xf4}; // CR4=660 -> ECX; CPUID; HLT
    memcpy(p,code,sizeof(code));
    bool ok=hv_vm_map(p,0,4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC)==0;
    const struct {unsigned field; uint64_t value;} state[]={
        {0x6800,0x23},{0x6804,0x2000},{0x6000,0xe0000020},{0x6004,0x40000023},
        {0x6002,0x2000},{0x6006,0},{0x4002,0x200080}};
    for(auto e:state) ok &= hv_vmx_vcpu_write_vmcs(c,e.field,e.value)==0;
    for(unsigned i=0;i<6;++i) {
        ok &= hv_vmx_vcpu_write_vmcs(c,0x800+i*2,i==1?8:16)==0;
        ok &= hv_vmx_vcpu_write_vmcs(c,0x4800+i*2,0xffffffff)==0;
        ok &= hv_vmx_vcpu_write_vmcs(c,0x4814+i*2,i==1?0xc09b:0xc093)==0;
    }
    check(ok,"configure Apple legacy CR0=23 and CR4 VMXE mask");
    auto r=hv_vcpu_run(c); uint64_t bx=0,cx=0,dx=0;
    check(r==0 && field(c,VMCS_RO_EXIT_REASON,10),"legacy CR0/CR3/CR4 initialization reaches CPUID");
    check(hv_vcpu_read_register(c,HV_X86_RBX,&bx)==0 && bx==0x40000033,"guest CR0 ET fixed one, masked CD preserved, forced TS hidden");
    check(hv_vcpu_read_register(c,HV_X86_RCX,&cx)==0 && cx==0x660 && field(c,VMCS_GUEST_CR4,0x2660),"CR4 preparation preserves host-owned VMXE privately");
    check(hv_vcpu_read_register(c,HV_X86_RDX,&dx)==0 && dx==0x800000 && field(c,VMCS_GUEST_CR3,0x800000),"nonpaged CR3 staging roundtrip");
    // A finite loop spanning many execution quanta must resume to HLT.
    const unsigned char loop[]={0xb9,0x88,0x13,0,0,0x49,0x75,0xfd,0xf4};
    memcpy(p,loop,sizeof(loop)); hv_vcpu_write_register(c,HV_X86_RIP,0);
    unsigned yields=0; bool halted=false;
    for(unsigned i=0;i<32;++i) {
        r=hv_vcpu_run(c); if(r) break;
        if(field(c,VMCS_RO_EXIT_REASON,12)) {halted=true; break;}
        if(!field(c,VMCS_RO_EXIT_REASON,1) || !field(c,VMCS_RO_VMEXIT_INSTR_LEN,0)) break;
        ++yields;
    }
    check(halted && yields>=2,"finite 5000-iteration loop survives multiple bounded host exits");
    // FPU context switching now permits initialization without touching host state.
    const unsigned char fpu[]={0xdb,0xe3,0xf4}; memcpy(p,fpu,sizeof(fpu));
    hv_vcpu_write_register(c,HV_X86_RIP,0);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,12),"guest FNINIT reaches HLT after CR4 OSFXSR");
    check(hv_vm_unmap(0,4096)==0,"unmap control guest"); free(p);
    check(hv_vcpu_destroy(c)==0 && hv_vm_destroy()==0,"control guest cleanup");
    return failures?1:0;
}
