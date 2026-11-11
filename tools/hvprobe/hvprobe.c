// hvprobe: call Hypervisor.framework the way QEMU/Docker do, print every return code.
// Run after `sudo sysctl debug.amdhv.takeover=1`, then read `sysctl -n debug.amdhv.log`.
#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

static const char *hvstr(hv_return_t r) {
    switch (r) {
        case HV_SUCCESS:      return "HV_SUCCESS";
        case HV_ERROR:        return "HV_ERROR";
        case HV_BUSY:         return "HV_BUSY";
        case HV_BAD_ARGUMENT: return "HV_BAD_ARGUMENT";
        case HV_NO_RESOURCES: return "HV_NO_RESOURCES";
        case HV_NO_DEVICE:    return "HV_NO_DEVICE";
        case HV_DENIED:       return "HV_DENIED";
        case HV_UNSUPPORTED:  return "HV_UNSUPPORTED";
        default:              return "?";
    }
}

static hv_return_t show(const char *what, hv_return_t r) {
    printf("%-55s -> 0x%08x %s\n", what, (unsigned)r, hvstr(r));
    return r;
}
#define CALL(expr) show(#expr, (expr))

int main(int argc, char **argv) {
    int expect_stubs = argc == 2 && strcmp(argv[1], "--expect-stubs") == 0;
    if (argc > 1 && !expect_stubs) {
        fprintf(stderr, "usage: %s [--expect-stubs]\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    int sup = -1;
    size_t len = sizeof(sup);
    sysctlbyname("kern.hv_support", &sup, &len, NULL, 0);
    printf("kern.hv_support = %d\n", sup);

    uint64_t v = 0;
    CALL(hv_vmx_read_capability(HV_VMX_CAP_PINBASED, &v));
    printf("    pinbased cap = 0x%llx\n", v);
    hv_return_t created = CALL(hv_vm_create(HV_VM_DEFAULT));
    if (expect_stubs) {
        if (created == HV_SUCCESS) CALL(hv_vm_destroy());
        if (sup != 1 || created != HV_UNSUPPORTED) {
            fprintf(stderr, "FAIL: expected hv_support=1 and vm_create=HV_UNSUPPORTED\n");
            return 1;
        }
        puts("PASS: phase 2 stub response");
        return 0;
    }
    if (created != HV_SUCCESS) return 1;

    void *mem = valloc(0x10000);
    memset(mem, 0xF4, 0x10000);   // hlt
    CALL(hv_vm_map(mem, 0x0, 0x10000, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));

    hv_vcpuid_t vcpu = 0;
    if (CALL(hv_vcpu_create(&vcpu, HV_VCPU_DEFAULT)) != HV_SUCCESS) {
        CALL(hv_vm_destroy());
        return 1;
    }
    printf("    vcpu = %u\n", vcpu);
    CALL(hv_vmx_vcpu_read_vmcs(vcpu, VMCS_GUEST_RIP, &v));
    CALL(hv_vcpu_run(vcpu));
    CALL(hv_vcpu_destroy(vcpu));
    CALL(hv_vm_unmap(0x0, 0x10000));
    CALL(hv_vm_destroy());
    return 0;
}
