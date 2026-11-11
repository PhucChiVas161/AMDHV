#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static unsigned failures;
static void check(bool ok,const char *name) { printf("%s: %s\n",ok?"PASS":"FAIL",name); failures+=!ok; }
static bool field(hv_vcpuid_t cpu,unsigned enc,uint64_t expected) {
    uint64_t value=~0ull; return hv_vmx_vcpu_read_vmcs(cpu,enc,&value)==0 && value==expected;
}
int main() {
    setvbuf(stdout,nullptr,_IONBF,0);
    if(hv_vm_create(HV_VM_DEFAULT)) return 2;
    hv_vcpuid_t cpu; if(hv_vcpu_create(&cpu,HV_VCPU_DEFAULT)) {hv_vm_destroy(); return 2;}
    void *page=nullptr; if(posix_memalign(&page,4096,4096)) return 2;
    auto b=static_cast<unsigned char*>(page); memset(b,0,4096);
    const unsigned char real[]={
        0x0f,0x01,0x16,0x00,0x01, // lgdt [100]
        0x0f,0x01,0x1e,0x06,0x01, // lidt [106]
        0x0f,0x01,0x06,0x10,0x01, // sgdt [110]
        0x0f,0x01,0x0e,0x16,0x01, // sidt [116]
        0x0f,0xa2,               // cpuid
        0x0f,0x20,0xc0,          // mov eax,cr0
        0x66,0x83,0xc8,0x01,     // or eax,1
        0x0f,0x22,0xc0,          // mov cr0,eax
        0x66,0xea,0x40,0,0,0,8,0 // jmp 8:40
    };
    const unsigned char prot[]={0x66,0xb8,0x10,0,0x8e,0xd8,0x8e,0xc0,0x8e,0xd0,
        0xc7,0x05,0,2,0,0,0x78,0x56,0x34,0x12,0x0f,0xa2,0xf4};
    memcpy(b,real,sizeof(real)); memcpy(b+0x40,prot,sizeof(prot));
    const unsigned char gdtr[]={23,0,0x80,1,0,0}, idtr[]={0xff,3,0,3,0,0};
    memcpy(b+0x100,gdtr,6); memcpy(b+0x106,idtr,6);
    uint64_t gdt[]={0,0x00cf9b000000ffffull,0x00cf93000000ffffull}; memcpy(b+0x180,gdt,sizeof(gdt));
    check(hv_vm_map(page,0,4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC)==0,"map descriptor guest");
    auto r=hv_vcpu_run(cpu);
    check(r==0 && field(cpu,VMCS_RO_EXIT_REASON,10),"LGDT/LIDT/SGDT/SIDT reach CPUID");
    check(!memcmp(b+0x110,gdtr,6) && !memcmp(b+0x116,idtr,6),"descriptor stores reflect guest tables");
    check(field(cpu,VMCS_GUEST_GDTR_BASE,0x180) && field(cpu,VMCS_GUEST_GDTR_LIMIT,23) &&
        field(cpu,VMCS_GUEST_IDTR_BASE,0x300) && field(cpu,VMCS_GUEST_IDTR_LIMIT,0x3ff),"table state published across exit");
    check(hv_vcpu_write_register(cpu,HV_X86_RIP,22)==0,"resume before CR0 PE transition");
    r=hv_vcpu_run(cpu);
    check(r==0 && field(cpu,VMCS_RO_EXIT_REASON,10) && field(cpu,VMCS_GUEST_CR0,0x11) &&
        field(cpu,VMCS_GUEST_CS,8),"PE enable and far jump execute protected32 code");
    uint32_t marker=0; memcpy(&marker,b+0x200,4);
    check(marker==0x12345678,"protected guest wrote expected marker through NPT");
    check(field(cpu,VMCS_GUEST_GDTR_BASE,0x180) && field(cpu,VMCS_GUEST_IDTR_BASE,0x300),"tables survive resume and mode transition");
    hv_vcpu_write_register(cpu,HV_X86_RIP,0x56);
    check(hv_vcpu_run(cpu)==0 && field(cpu,VMCS_RO_EXIT_REASON,12),"resume protected guest reaches HLT");
    check(hv_vcpu_destroy(cpu)==0 && hv_vcpu_create(&cpu,HV_VCPU_DEFAULT)==0,"fresh vCPU for descriptor fault");
    b[3]=0; b[4]=0x20; // LGDT operand at unmapped GPA2000
    check(hv_vcpu_run(cpu)==0 && field(cpu,VMCS_RO_EXIT_REASON,48),"unmapped LGDT operand is blocked by NPT");
    check(field(cpu,VMCS_GUEST_GDTR_BASE,0) && field(cpu,VMCS_GUEST_GDTR_LIMIT,0xffff),"faulting LGDT leaves descriptor state unchanged");
    check(hv_vm_unmap(0,4096)==0,"unmap descriptor guest"); free(page);
    check(hv_vcpu_destroy(cpu)==0 && hv_vm_destroy()==0,"descriptor guest cleanup");
    return failures?1:0;
}
