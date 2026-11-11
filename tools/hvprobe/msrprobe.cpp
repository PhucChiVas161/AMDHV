#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static unsigned failures;
static void check(bool pass, const char *name) { printf("%s: %s\n", pass ? "PASS" : "FAIL", name); failures += !pass; }
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (hv_vm_create(HV_VM_DEFAULT)) return 2;
    hv_vcpuid_t cpu;
    if (hv_vcpu_create(&cpu, HV_VCPU_DEFAULT)) { hv_vm_destroy(); return 2; }
    void *page = nullptr;
    if (posix_memalign(&page, 4096, 4096)) { hv_vcpu_destroy(cpu); hv_vm_destroy(); return 2; }
    memset(page, 0, 4096);
    // mov ecx,CSTAR; mov eax,magic; xor edx,edx; wrmsr; xor eax,eax; rdmsr; cpuid
    const unsigned char code[] = {0x66,0xb9,0x83,0,0,0xc0,0x66,0xb8,0x78,0x56,0x34,0x12,
        0x66,0x31,0xd2,0x0f,0x30,0x66,0x31,0xc0,0x0f,0x32,0x0f,0xa2};
    memcpy(page, code, sizeof(code));
    check(hv_vm_map(page, 0, 4096, HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC) == HV_SUCCESS, "map MSR guest");
    for (unsigned id = 1; id <= 5; ++id) {
        uint32_t address = id <= 4 ? 0xc0000080u + id : 0xc0000102u;
        check(hv_vcpu_enable_managed_msr(cpu, address, true) == HV_SUCCESS, "enable syscall MSR");
        check(hv_vcpu_write_msr(cpu, address, 0x1000 + id) == HV_SUCCESS, "write MSR cache");
    }
    check(hv_vcpu_enable_managed_msr(cpu, 0xc0010117, true) != HV_SUCCESS, "reject host HSAVE MSR");
    auto result = hv_vcpu_run(cpu);
    uint64_t reason=0, value=0;
    check(result == HV_SUCCESS && hv_vmx_vcpu_read_vmcs(cpu, VMCS_RO_EXIT_REASON, &reason) == HV_SUCCESS && reason == 10, "managed WRMSR/RDMSR reaches CPUID");
    check(hv_vcpu_read_register(cpu, HV_X86_RAX, &value) == HV_SUCCESS && value == 0x12345678, "guest reads its CSTAR value");
    check(hv_vcpu_read_msr(cpu, 0xc0000083, &value) == HV_SUCCESS && value == 0x12345678, "guest write published to userspace");
    check(hv_vcpu_enable_managed_msr(cpu, 0xc0000083, false) == HV_SUCCESS, "disable managed CSTAR");
    check(hv_vcpu_write_register(cpu, HV_X86_RIP, 15) == HV_SUCCESS, "rewind to WRMSR");
    result = hv_vcpu_run(cpu);
    check(result == HV_SUCCESS && hv_vmx_vcpu_read_vmcs(cpu, VMCS_RO_EXIT_REASON, &reason) == HV_SUCCESS && reason == 32, "disabled WRMSR exits to userspace");
    check(hv_vmx_vcpu_read_vmcs(cpu, VMCS_GUEST_RIP, &value) == HV_SUCCESS && value == 15, "unhandled MSR does not advance RIP");
    check(hv_vcpu_enable_managed_msr(cpu, 0xc0000083, true) == HV_SUCCESS, "re-enable CSTAR");
    check(hv_vcpu_set_msr_access(cpu, 0xc0000083, HV_MSR_READ) == HV_SUCCESS, "set read-only MSR access");
    hv_vcpu_write_register(cpu, HV_X86_RIP, 15);
    check(hv_vcpu_run(cpu) == HV_SUCCESS && hv_vmx_vcpu_read_vmcs(cpu, VMCS_RO_EXIT_REASON, &reason) == HV_SUCCESS && reason == 32, "read-only permission exits on WRMSR");
    hv_vcpu_write_register(cpu, HV_X86_RIP, 20);
    check(hv_vcpu_run(cpu) == HV_SUCCESS && hv_vmx_vcpu_read_vmcs(cpu, VMCS_RO_EXIT_REASON, &reason) == HV_SUCCESS && reason == 10, "read-only permission emulates RDMSR");
    check(hv_vcpu_set_msr_access(cpu, 0xc0000083, HV_MSR_WRITE) == HV_SUCCESS, "set write-only MSR access");
    hv_vcpu_write_register(cpu, HV_X86_RIP, 20);
    check(hv_vcpu_run(cpu) == HV_SUCCESS && hv_vmx_vcpu_read_vmcs(cpu, VMCS_RO_EXIT_REASON, &reason) == HV_SUCCESS && reason == 31, "write-only permission exits on RDMSR");
    check(hv_vcpu_set_msr_access(cpu, 0xc0000083, 8) != HV_SUCCESS, "reject unknown access flags");
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t address = 0xc0000100u + i;
        unsigned field = 0x680e + i*2;
        check(hv_vcpu_enable_managed_msr(cpu, address, true) == HV_SUCCESS, "enable FS/GS base MSR alias");
        check(hv_vcpu_set_msr_access(cpu, address, HV_MSR_READ | HV_MSR_WRITE) == HV_SUCCESS, "allow FS/GS read and write");
        check(hv_vcpu_write_msr(cpu, address, 0x3000+i*4096) == HV_SUCCESS, "write FS/GS through MSR API");
        check(hv_vmx_vcpu_read_vmcs(cpu, field, &value) == HV_SUCCESS && value == 0x3000+i*4096, "MSR write visible in VMCS segment base");
        hv_vcpu_write_register(cpu, HV_X86_RCX, address);
        hv_vcpu_write_register(cpu, HV_X86_RAX, 0x5000+i*4096);
        hv_vcpu_write_register(cpu, HV_X86_RDX, 0);
        hv_vcpu_write_register(cpu, HV_X86_RIP, 15);
        check(hv_vcpu_run(cpu) == HV_SUCCESS && hv_vmx_vcpu_read_vmcs(cpu, VMCS_RO_EXIT_REASON, &reason) == HV_SUCCESS && reason == 10, "guest WRMSR/RDMSR FS/GS reaches CPUID");
        check(hv_vcpu_read_register(cpu, HV_X86_RAX, &value) == HV_SUCCESS && value == 0x5000+i*4096, "guest reads updated FS/GS base");
        check(hv_vcpu_read_msr(cpu, address, &value) == HV_SUCCESS && value == 0x5000+i*4096, "FS/GS saved base visible to userspace");
        check(hv_vmx_vcpu_write_vmcs(cpu, field, 0) == HV_SUCCESS, "restore zero FS/GS base");
    }
    const unsigned sysenterFields[] = {0x482a, 0x6824, 0x6826};
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t address = 0x174 + i;
        check(hv_vcpu_enable_managed_msr(cpu, address, true) == HV_SUCCESS, "enable SYSENTER MSR");
        check(hv_vcpu_set_msr_access(cpu, address, HV_MSR_READ | HV_MSR_WRITE) == HV_SUCCESS, "allow SYSENTER read/write");
        check(hv_vcpu_write_msr(cpu, address, 0x6000+i*4096) == HV_SUCCESS, "write SYSENTER MSR alias");
        check(hv_vmx_vcpu_read_vmcs(cpu, sysenterFields[i], &value) == HV_SUCCESS && value == 0x6000+i*4096, "SYSENTER API value matches VMCS");
        hv_vcpu_write_register(cpu, HV_X86_RCX, address);
        hv_vcpu_write_register(cpu, HV_X86_RAX, 0x9000+i*4096);
        hv_vcpu_write_register(cpu, HV_X86_RDX, 0);
        hv_vcpu_write_register(cpu, HV_X86_RIP, 15);
        check(hv_vcpu_run(cpu) == HV_SUCCESS && hv_vmx_vcpu_read_vmcs(cpu, VMCS_RO_EXIT_REASON, &reason) == HV_SUCCESS && reason == 10, "guest SYSENTER MSR write/read reaches CPUID");
        check(hv_vcpu_read_register(cpu, HV_X86_RAX, &value) == HV_SUCCESS && value == 0x9000+i*4096, "guest reads updated SYSENTER MSR");
        check(hv_vcpu_read_msr(cpu, address, &value) == HV_SUCCESS && value == 0x9000+i*4096, "saved SYSENTER value visible to userspace");
        check(hv_vmx_vcpu_write_vmcs(cpu, sysenterFields[i], 0) == HV_SUCCESS, "clear SYSENTER MSR");
    }
    check(hv_vm_unmap(0, 4096) == HV_SUCCESS, "unmap MSR guest");
    check(hv_vcpu_destroy(cpu) == HV_SUCCESS, "destroy MSR vCPU");
    check(hv_vm_destroy() == HV_SUCCESS, "destroy MSR VM");
    free(page);
    return failures ? 1 : 0;
}
