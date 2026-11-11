#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (hv_vm_create(HV_VM_DEFAULT)) return 2;
    hv_vcpuid_t cpu;
    auto result = hv_vcpu_create(&cpu, HV_VCPU_TSC_RELATIVE);
    printf("create relative-TSC vCPU: 0x%x\n", result);
    if (result) { hv_vm_destroy(); return 1; }
    void *page = nullptr;
    if (posix_memalign(&page, 4096, 4096)) { hv_vcpu_destroy(cpu); hv_vm_destroy(); return 2; }
    memset(page, 0, 4096);
    const unsigned char code[] = {0x0f,0x31,0x0f,0xa2}; // rdtsc; cpuid (intercept before clobber)
    memcpy(page, code, sizeof(code));
    bool ok = hv_vm_map(page, 0, 4096, HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC) == HV_SUCCESS;
    for (int64_t offset : {int64_t(0), int64_t(1000000000), int64_t(-1000000000)}) {
        bool pass = hv_vcpu_set_tsc_relative(cpu, offset) == HV_SUCCESS;
        pass &= hv_vcpu_write_register(cpu, HV_X86_RIP, 0) == HV_SUCCESS;
        uint64_t before = hv_tsc_clock();
        result = hv_vcpu_run_until(cpu, HV_DEADLINE_FOREVER);
        uint64_t after = hv_tsc_clock(), lo=0, hi=0, reason=0;
        pass &= result == HV_SUCCESS;
        pass &= hv_vcpu_read_register(cpu, HV_X86_RAX, &lo) == HV_SUCCESS;
        pass &= hv_vcpu_read_register(cpu, HV_X86_RDX, &hi) == HV_SUCCESS;
        pass &= hv_vmx_vcpu_read_vmcs(cpu, VMCS_RO_EXIT_REASON, &reason) == HV_SUCCESS && reason == 10;
        uint64_t guest = ((hi & 0xffffffffull) << 32) | (lo & 0xffffffffull);
        uint64_t hostEquivalent = guest - (uint64_t)offset;
        pass &= hostEquivalent >= before && hostEquivalent <= after;
        printf("%s: RDTSC relative offset=%lld result=0x%x guest=%llu host=[%llu,%llu]\n",
            pass ? "PASS" : "FAIL", offset, result, guest, before, after);
        ok &= pass;
    }
    auto check = [&](bool pass, const char *label) { printf("%s: %s\n", pass ? "PASS" : "FAIL", label); ok &= pass; };
    check(hv_vcpu_run_until(cpu, 0) == HV_UNSUPPORTED, "finite deadline rejected without preemption-timer support");
    const uint64_t target = 0x1234000000000ull;
    uint64_t before = hv_tsc_clock();
    bool pass = hv_vcpu_write_msr(cpu, 0x10, target) == HV_SUCCESS;
    pass &= hv_vcpu_write_register(cpu, HV_X86_RIP, 0) == HV_SUCCESS;
    pass &= hv_vcpu_run_until(cpu, HV_DEADLINE_FOREVER) == HV_SUCCESS;
    uint64_t after = hv_tsc_clock(), lo=0, hi=0, snapshot=0;
    pass &= hv_vcpu_read_register(cpu, HV_X86_RAX, &lo) == HV_SUCCESS;
    pass &= hv_vcpu_read_register(cpu, HV_X86_RDX, &hi) == HV_SUCCESS;
    uint64_t value = (hi << 32) | (lo & 0xffffffffull);
    pass &= value >= target && value - target <= after - before;
    pass &= hv_vcpu_read_msr(cpu, 0x10, &snapshot) == HV_SUCCESS && snapshot >= value && snapshot - target <= after - before;
    check(pass, "userspace TSC write, guest RDTSC and shared snapshot agree");
    check(hv_vcpu_enable_managed_msr(cpu, 0x10, true) == HV_SUCCESS, "enable managed TSC");
    check(hv_vcpu_set_msr_access(cpu, 0x10, HV_MSR_READ|HV_MSR_WRITE) == HV_SUCCESS, "allow guest TSC read/write");
    const unsigned char msrCode[] = {0x0f,0x30,0x0f,0x32,0x0f,0xa2}; // wrmsr; rdmsr; cpuid
    memcpy(page, msrCode, sizeof(msrCode));
    hv_vcpu_write_register(cpu, HV_X86_RCX, 0x10);
    hv_vcpu_write_register(cpu, HV_X86_RAX, target & 0xffffffff);
    hv_vcpu_write_register(cpu, HV_X86_RDX, target >> 32);
    hv_vcpu_write_register(cpu, HV_X86_RIP, 0);
    before = hv_tsc_clock(); result = hv_vcpu_run_until(cpu, HV_DEADLINE_FOREVER); after = hv_tsc_clock();
    uint64_t reason=0;
    pass = result == HV_SUCCESS && hv_vmx_vcpu_read_vmcs(cpu, VMCS_RO_EXIT_REASON, &reason) == HV_SUCCESS && reason == 10;
    pass &= hv_vcpu_read_register(cpu, HV_X86_RAX, &lo) == HV_SUCCESS;
    pass &= hv_vcpu_read_register(cpu, HV_X86_RDX, &hi) == HV_SUCCESS;
    value = (hi << 32) | (lo & 0xffffffffull);
    pass &= value >= target && value - target <= after - before;
    check(pass, "guest WRMSR TSC changes only guest clock, RDMSR continues to CPUID");
    check(hv_vcpu_set_msr_access(cpu, 0x10, HV_MSR_READ) == HV_SUCCESS, "deny guest TSC writes");
    hv_vcpu_write_register(cpu, HV_X86_RIP, 0);
    check(hv_vcpu_run_until(cpu, HV_DEADLINE_FOREVER) == HV_SUCCESS &&
          hv_vmx_vcpu_read_vmcs(cpu, VMCS_RO_EXIT_REASON, &reason) == HV_SUCCESS && reason == 32, "denied TSC WRMSR exits");
    ok &= hv_vm_unmap(0, 4096) == HV_SUCCESS;
    ok &= hv_vcpu_destroy(cpu) == HV_SUCCESS;
    ok &= hv_vm_destroy() == HV_SUCCESS;
    free(page);
    return ok ? 0 : 1;
}
