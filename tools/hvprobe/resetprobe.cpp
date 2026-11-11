#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static unsigned failures;
static void check(bool ok,const char *label) { printf("%s: %s\n",ok?"PASS":"FAIL",label); failures+=!ok; }
int main() {
    setvbuf(stdout,nullptr,_IONBF,0);
    if(hv_vm_create(HV_VM_DEFAULT)) return 2;
    hv_vcpuid_t cpu;
    if(hv_vcpu_create(&cpu,HV_VCPU_TSC_RELATIVE)) { hv_vm_destroy(); return 2; }
    void *page=nullptr; if(posix_memalign(&page,4096,4096)) return 2;
    memset(page,0,4096);
    // Reset vector: MOV EAX,CR0; MOV EBX,CR4; CPUID; MOV CR0,EAX; HLT.
    const unsigned char code[]={0x0f,0x20,0xc0,0x0f,0x20,0xe3,0x0f,0xa2,0x0f,0x22,0xc0,0xf4};
    memcpy(static_cast<char*>(page)+0xff0,code,sizeof(code));
    bool ok=hv_vm_map(page,0xfffff000,4096,HV_MEMORY_READ|HV_MEMORY_EXEC)==0;
    const struct { unsigned field; uint64_t value; } state[]={
        {VMCS_GUEST_CR0,0x30},{VMCS_GUEST_CR4,0x2000},
        {VMCS_CTRL_CR0_MASK,0xe0000020},{VMCS_CTRL_CR0_SHADOW,0x60000010},
        {VMCS_CTRL_CR4_MASK,0x2000},{VMCS_CTRL_CR4_SHADOW,0},
        {VMCS_CTRL_CPU_BASED,0x200080},{VMCS_GUEST_CS,0xf000},
        {VMCS_GUEST_CS_BASE,0xffff0000},{VMCS_GUEST_RIP,0xfff0}};
    for(auto entry:state) ok &= hv_vmx_vcpu_write_vmcs(cpu,entry.field,entry.value)==0;
    check(ok,"configure exact native reset CRs, controls and high ROM vector");
    uint64_t reason=0,rax=0,rbx=0,rip=0,qual=0,length=0;
    auto r=hv_vcpu_run_until(cpu,HV_DEADLINE_FOREVER);
    check(r==0 && hv_vmx_vcpu_read_vmcs(cpu,VMCS_RO_EXIT_REASON,&reason)==0 && reason==10,"reset vector executes CR reads and reaches CPUID");
    check(hv_vcpu_read_register(cpu,HV_X86_RAX,&rax)==0 && rax==0x60000010,"guest reads CR0 shadow, without forced host TS/MP");
    check(hv_vcpu_read_register(cpu,HV_X86_RBX,&rbx)==0 && rbx==0,"guest CR4 hides host-owned VMXE");
    hv_vcpu_write_register(cpu,HV_X86_RIP,0xfff8); hv_vcpu_write_register(cpu,HV_X86_RAX,0x10);
    r=hv_vcpu_run(cpu);
    check(r==0 && hv_vmx_vcpu_read_vmcs(cpu,VMCS_RO_EXIT_REASON,&reason)==0 && reason==28 &&
        hv_vmx_vcpu_read_vmcs(cpu,VMCS_RO_EXIT_QUALIFIC,&qual)==0 && qual==0 &&
        hv_vmx_vcpu_read_vmcs(cpu,VMCS_RO_VMEXIT_INSTR_LEN,&length)==0 && length==3 &&
        hv_vcpu_read_register(cpu,HV_X86_RIP,&rip)==0 && rip==0xfff8,"masked CR0 change returns exact MOV-CR exit without executing it");
    hv_vcpu_write_register(cpu,HV_X86_RAX,0x60000010);
    r=hv_vcpu_run(cpu);
    check(r==0 && hv_vmx_vcpu_read_vmcs(cpu,VMCS_RO_EXIT_REASON,&reason)==0 && reason==12,"unchanged CR0 write is consumed and reaches HLT");
    hv_vcpu_write_register(cpu,HV_X86_RIP,0xfff8); hv_vcpu_write_register(cpu,HV_X86_RAX,0x60000014);
    check(hv_vcpu_run(cpu)==HV_UNSUPPORTED,"unimplemented CR0 EM change fails closed");
    check(hv_vm_unmap(0xfffff000,4096)==0,"unmap reset ROM"); free(page);
    check(hv_vcpu_destroy(cpu)==0 && hv_vm_destroy()==0,"reset probe cleanup");
    return failures?1:0;
}
