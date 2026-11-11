// Probe all startup MSRs seen in the installed Apple XPC VM, without executing a guest.
#include <Hypervisor/hv.h>
#include <cstdio>
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (hv_vm_create(HV_VM_DEFAULT)) return 2;
    hv_vcpuid_t cpu;
    if (hv_vcpu_create(&cpu, HV_VCPU_TSC_RELATIVE)) { hv_vm_destroy(); return 2; }
    unsigned failed = 0;
    const uint32_t msrs[] = {0xc0000083,0xc0000084,0xc0000100,0xc0000101,0xc0000102,
        0xc0000082,0xc0000081,0x174,0x176,0x175,0x10};
    for (auto msr : msrs) {
        auto enable = hv_vcpu_enable_managed_msr(cpu, msr, true);
        auto access = hv_vcpu_set_msr_access(cpu, msr, HV_MSR_READ|HV_MSR_WRITE);
        printf("MSR 0x%08x enable=0x%x access=0x%x\n", msr, enable, access);
        failed += enable != HV_SUCCESS || access != HV_SUCCESS;
    }
    bool cleaned = hv_vcpu_destroy(cpu) == HV_SUCCESS;
    cleaned &= hv_vm_destroy() == HV_SUCCESS;
    printf("%s: native startup MSR group (%u missing), cleanup=%s; no VM execution\n", failed ? "FAIL" : "PASS", failed, cleaned ? "PASS" : "FAIL");
    return failed || !cleaned ? 1 : 0;
}
